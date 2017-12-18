/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "codec"
#include <inttypes.h>
#include <utils/Log.h>

#include "SimplePlayer.h"

#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <media/ICrypto.h>
#include <media/IMediaHTTPService.h>
#include <media/IMediaPlayerService.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AString.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecList.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/NuMediaExtractor.h>
#include <OMX_IVCommon.h>
#include <gui/ISurfaceComposer.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/Surface.h>
#include <ui/DisplayInfo.h>
#include <errno.h>

//#include <libavcodec/avcodec.h>
//#include <libavformat/avformat.h>
//#include <libavutil/opt.h>
//#include <libavutil/pixdesc.h>

#include "opengl_manager.h"

static void usage(const char *me) {
    fprintf(stderr, "usage: %s [-a] use audio\n"
                    "\t\t[-v] use video\n"
                    "\t\t[-p] playback\n"
                    "\t\t[-S] allocate buffers from a surface\n"
                    "\t\t[-R] render output to surface (enables -S)\n"
                    "\t\t[-T] use render timestamps (enables -R)\n",
                    me);
    exit(1);
}

namespace android {

struct CodecState {
    sp<MediaCodec> mCodec;
    Vector<sp<ABuffer> > mInBuffers;
    Vector<sp<ABuffer> > mOutBuffers;
    bool mSignalledInputEOS;
    bool mSawOutputEOS;
    int64_t mNumBuffersDecoded;
    int64_t mNumBytesDecoded;
    bool mIsAudio;
};

}  // namespace android


/**
 * Packs YUV420 frame by moving it to a smaller size buffer with stride and slice
 * height equal to the original frame width and height.
 */
static char* PackYUV420(int width, int height,
        int stride, int sliceHeight, char *src) {
   char *dst = (char *)malloc(width * height * 3 / 2);
    // Y copy.
    for (int i = 0; i < height; i++) {
        memcpy(dst +  i * width, src +  i * stride, width);
    }
    // U and V copy.
    int u_src_offset = stride * sliceHeight;
    int v_src_offset = u_src_offset + u_src_offset / 4;
    int u_dst_offset = width * height;
    int v_dst_offset = u_dst_offset + u_dst_offset / 4;
    for (int i = 0; i < height / 2; i++) {
         memcpy(dst + u_dst_offset + i * (width / 2),  src + u_src_offset + i * (stride / 2), width / 2 );
         memcpy( dst + v_dst_offset + i * (width / 2),  src + v_src_offset + i * (stride / 2),width / 2);
    }
    
    return dst;
}

static int decode(
        const android::sp<android::ALooper> &looper,
        const char *path,
        bool useAudio,
        bool useVideo,
        const android::sp<android::Surface> &surface,
        bool renderSurface,
        bool useTimestamp) 
{
    using namespace android;
    FILE *pf = NULL;
    static int64_t kTimeout = 500ll;

    sp<NuMediaExtractor> extractor = new NuMediaExtractor;
    if (extractor->setDataSource(NULL /* httpService */, path) != OK) {
        fprintf(stderr, "unable to instantiate extractor.\n");
        return 1;
    }

    int frameWidth = 0;
    int frameHeight = 0;
    int frameColorFormat  = 0;
    int frameStride = 0;
    int frameSliceHeight = 0;
    char yuv_name[64];
    OPENGL_MGR_HANDLE context = NULL;
    
    memset(yuv_name, 0, sizeof(yuv_name));
    
    KeyedVector<size_t, CodecState> stateByTrack;

    bool haveAudio = false;
    bool haveVideo = false;
    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<AMessage> format;
        status_t err = extractor->getTrackFormat(i, &format);
        CHECK_EQ(err, (status_t)OK);

        AString mime;
        CHECK(format->findString("mime", &mime));

        bool isAudio = !strncasecmp(mime.c_str(), "audio/", 6);
        bool isVideo = !strncasecmp(mime.c_str(), "video/", 6);

        if (useAudio && !haveAudio && isAudio) {
            haveAudio = true;
            continue;
        } else if (useVideo && !haveVideo && isVideo) {
            haveVideo = true;
        } else {
            continue;
        }

        printf("selecting track %zu\n", i);

        err = extractor->selectTrack(i);
        CHECK_EQ(err, (status_t)OK);

        CodecState *state =
            &stateByTrack.editValueAt(stateByTrack.add(i, CodecState()));

        state->mNumBytesDecoded = 0;
        state->mNumBuffersDecoded = 0;
        state->mIsAudio = isAudio;

        state->mCodec = MediaCodec::CreateByType(
                looper, mime.c_str(), false /* encoder */);

        CHECK(state->mCodec != NULL);

        err = state->mCodec->configure(
                format, isVideo ? surface : NULL,
                NULL /* crypto */,
                0 /* flags */);

        CHECK_EQ(err, (status_t)OK);

        state->mSignalledInputEOS = false;
        state->mSawOutputEOS = false;
    }

    CHECK(!stateByTrack.isEmpty());

    int64_t startTimeUs = ALooper::GetNowUs();
    int64_t startTimeRender = -1;

    for (size_t i = 0; i < stateByTrack.size(); ++i) {
        CodecState *state = &stateByTrack.editValueAt(i);

        sp<MediaCodec> codec = state->mCodec;

        CHECK_EQ((status_t)OK, codec->start());

        CHECK_EQ((status_t)OK, codec->getInputBuffers(&state->mInBuffers));
        CHECK_EQ((status_t)OK, codec->getOutputBuffers(&state->mOutBuffers));

        printf("got %zu input and %zu output buffers\n",
              state->mInBuffers.size(), state->mOutBuffers.size());
    }

    bool sawInputEOS = false;

    for (;;) {
        if (!sawInputEOS) {
            size_t trackIndex;
            status_t err = extractor->getSampleTrackIndex(&trackIndex);

            if (err != OK) {
                printf("saw input eos\n");
                sawInputEOS = true;
            } else {
                CodecState *state = &stateByTrack.editValueFor(trackIndex);

                size_t index;
                err = state->mCodec->dequeueInputBuffer(&index, kTimeout);

                if (err == OK) {
                    //printf("filling input buffer %zu\n", index);

                    const sp<ABuffer> &buffer = state->mInBuffers.itemAt(index);

                    err = extractor->readSampleData(buffer);
                    CHECK_EQ(err, (status_t)OK);

                    int64_t timeUs;
                    err = extractor->getSampleTime(&timeUs);
                    CHECK_EQ(err, (status_t)OK);

                    uint32_t bufferFlags = 0;

                    err = state->mCodec->queueInputBuffer(
                            index,
                            0 /* offset */,
                            buffer->size(),
                            timeUs,
                            bufferFlags);

                    CHECK_EQ(err, (status_t)OK);

                    extractor->advance();
                } else {
                    CHECK_EQ(err, -EAGAIN);
                }
            }
        } else {
            for (size_t i = 0; i < stateByTrack.size(); ++i) {
                CodecState *state = &stateByTrack.editValueAt(i);

                if (!state->mSignalledInputEOS) {
                    size_t index;
                    status_t err =
                        state->mCodec->dequeueInputBuffer(&index, kTimeout);

                    if (err == OK) {
                        printf("signalling input EOS on track %zu \n", i);

                        err = state->mCodec->queueInputBuffer(
                                index,
                                0 /* offset */,
                                0 /* size */,
                                0ll /* timeUs */,
                                MediaCodec::BUFFER_FLAG_EOS);

                        CHECK_EQ(err, (status_t)OK);

                        state->mSignalledInputEOS = true;
                    } else {
                        CHECK_EQ(err, -EAGAIN);
                    }
                }
            }
        }

        bool sawOutputEOSOnAllTracks = true;
        for (size_t i = 0; i < stateByTrack.size(); ++i) {
            CodecState *state = &stateByTrack.editValueAt(i);
            if (!state->mSawOutputEOS) {
                sawOutputEOSOnAllTracks = false;
                break;
            }
        }

        if (sawOutputEOSOnAllTracks) {
            break;
        }

        for (size_t i = 0; i < stateByTrack.size(); ++i) {
            CodecState *state = &stateByTrack.editValueAt(i);

            if (state->mSawOutputEOS) {
                continue;
            }

            size_t index;
            size_t offset;
            size_t size;
            int64_t presentationTimeUs;
            uint32_t flags;
            status_t err = state->mCodec->dequeueOutputBuffer(
                    &index, &offset, &size, &presentationTimeUs, &flags,
                    kTimeout);

            if (err == OK) {

                if (size  > 0) {
                // Save decoder output to yuv file.
                    char *frame = (char *)malloc(size);
                    sp<ABuffer> buffer = state->mOutBuffers.itemAt(index);
                    char *data = (char *)buffer->base() + offset;
                    int len = size;

                    memcpy(frame,  data, size);
                    // Convert NV12 to YUV420 if necessary.
                    if (frameColorFormat !=  OMX_COLOR_FormatYUV420Planar) {
                        //frame = NV12ToYUV420(frameWidth, frameHeight,
                        //        frameStride, frameSliceHeight, frame);
                        printf("unsurpported ....................\n");
                    }

                    int writeLength = frameWidth * frameHeight * 3 / 2 < size ? frameWidth * frameHeight * 3 / 2 : size;
                    // Pack frame if necessary.
                    if (writeLength < size  &&
                            (frameStride > frameWidth || frameSliceHeight > frameHeight)) {
                        free(frame);
                        frame = PackYUV420(frameWidth, frameHeight,
                                frameStride, frameSliceHeight, frame);
                    }
#if 1
                    if(NULL == context)
                    {
                        context = initOpenGL(frameWidth, frameHeight);
                    }

                    if(NULL != frame)
                    {
                        opengl_manager_push_data(context, frame);
                        free(frame);
                        frame = NULL;
                    }
#else
                    pf = fopen(yuv_name, "ab");
                    //pf = fopen("1280x736_420p.yuv", "ab");
                    if (pf){
                        int length = fwrite(frame, 1, writeLength, pf);
                        printf("length: %d\n", writeLength);
                        fclose(pf);
                    } else {
                        printf("open (%s) error: %s\n", yuv_name, strerror(errno));
                    }
#endif

                }

            //printf("draining output buffer %zu, time = %lld us\n", index, (long long)presentationTimeUs);
            ++state->mNumBuffersDecoded;
            state->mNumBytesDecoded += size;

            if (surface == NULL || !renderSurface) {
                err = state->mCodec->releaseOutputBuffer(index);
            } else if (useTimestamp) {
                if (startTimeRender == -1) {
                    // begin rendering 2 vsyncs (~33ms) after first decode
                    startTimeRender =
                            systemTime(SYSTEM_TIME_MONOTONIC) + 33000000
                            - (presentationTimeUs * 1000);
                }
                presentationTimeUs =
                        (presentationTimeUs * 1000) + startTimeRender;
                err = state->mCodec->renderOutputBufferAndRelease(
                        index, presentationTimeUs);
            } else {
                err = state->mCodec->renderOutputBufferAndRelease(index);
            }

            CHECK_EQ(err, (status_t)OK);

            if (flags & MediaCodec::BUFFER_FLAG_EOS) {
                printf("reached EOS on output.\n");

                state->mSawOutputEOS = true;
            }
            } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
                printf("INFO_OUTPUT_BUFFERS_CHANGED\n");
                CHECK_EQ((status_t)OK, state->mCodec->getOutputBuffers(&state->mOutBuffers));
                
                printf("got %zu output buffers\n", state->mOutBuffers.size());
            } else if (err == INFO_FORMAT_CHANGED) {
                sp<AMessage> format;
                CHECK_EQ((status_t)OK, state->mCodec->getOutputFormat(&format));
                format->findInt32("width", &frameWidth);
                format->findInt32("height", &frameHeight);
                format->findInt32("color-format", &frameColorFormat);
                if (! format->findInt32("stride", &frameStride)){
                    frameStride = frameWidth;
                }
                
                if (! format->findInt32("slice-height", &frameSliceHeight)){
                    frameSliceHeight = frameHeight;
                }

                frameStride = frameWidth < frameStride?frameStride: frameWidth;
                frameSliceHeight = frameHeight < frameSliceHeight?frameSliceHeight: frameHeight;
                
                AString mime;
                if (format->findString("mime", &mime)){
                    printf("mime.c_str(): %s\n", mime.c_str());
                    if (strstr(mime.c_str(), "video")) {
                        memset(yuv_name, 0, sizeof(yuv_name));
                        snprintf(yuv_name, sizeof(yuv_name) - 1, "%dx%d_420p.yuv", frameWidth, frameHeight);
                        remove(yuv_name);
                    }
                    
                }
          
                printf("INFO_FORMAT_CHANGED: %s\n", format->debugString().c_str());
            } else {
                CHECK_EQ(err, -EAGAIN);
            }
        }
    }

    int64_t elapsedTimeUs = ALooper::GetNowUs() - startTimeUs;

    for (size_t i = 0; i < stateByTrack.size(); ++i) {
        CodecState *state = &stateByTrack.editValueAt(i);

        CHECK_EQ((status_t)OK, state->mCodec->release());

        if (state->mIsAudio) {
            printf("track %zu: %lld bytes received. %.2f KB/sec\n",
                   i,
                   (long long)state->mNumBytesDecoded,
                   state->mNumBytesDecoded * 1E6 / 1024 / elapsedTimeUs);
        } else {
            printf("track %zu: %lld frames decoded, %.2f fps. %lld"
                    " bytes received. %.2f KB/sec\n",
                   i,
                   (long long)state->mNumBuffersDecoded,
                   state->mNumBuffersDecoded * 1E6 / elapsedTimeUs,
                   (long long)state->mNumBytesDecoded,
                   state->mNumBytesDecoded * 1E6 / 1024 / elapsedTimeUs);
        }
    }

    if(NULL != context)
    {
        uninitOpenGL(&context);
    }

    return 0;
}

int main(int argc, char **argv) {
    using namespace android;

    const char *me = argv[0];

    bool useAudio = false;
    bool useVideo = false;
    bool playback = false;
    bool useSurface = false;
    bool renderSurface = false;
    bool useTimestamp = false;

    int res;
    while ((res = getopt(argc, argv, "havpSDRT")) >= 0) {
        switch (res) {
            case 'a':
            {
                useAudio = true;
                break;
            }
            case 'v':
            {
                useVideo = true;
                break;
            }
            case 'p':
            {
                playback = true;
                break;
            }
            case 'T':
            {
                useTimestamp = true;
            }
            // fall through
            case 'R':
            {
                renderSurface = true;
            }
            // fall through
            case 'S':
            {
                useSurface = true;
                break;
            }
            case '?':
            case 'h':
            default:
            {
                usage(me);
            }
        }
    }

    argc -= optind;
    argv += optind;

    if (argc != 1) {
        usage(me);
    }

    if (!useAudio && !useVideo) {
        useAudio = useVideo = true;
    }

    ProcessState::self()->startThreadPool();

    DataSource::RegisterDefaultSniffers();

    sp<ALooper> looper = new ALooper;
    looper->start();

    sp<SurfaceComposerClient> composerClient;
    sp<SurfaceControl> control;
    sp<Surface> surface;

    if (playback || (useSurface && useVideo)) {
        printf("1. ########\n");
        composerClient = new SurfaceComposerClient;
        CHECK_EQ(composerClient->initCheck(), (status_t)OK);

        sp<IBinder> display(SurfaceComposerClient::getBuiltInDisplay(
                ISurfaceComposer::eDisplayIdMain));
        DisplayInfo info;
        SurfaceComposerClient::getDisplayInfo(display, &info);
        ssize_t displayWidth = info.w;
        ssize_t displayHeight = info.h;

        printf("display is %zd x %zd\n", displayWidth, displayHeight);

        control = composerClient->createSurface(
                String8("A Surface"),
                displayWidth,
                displayHeight,
                PIXEL_FORMAT_RGB_565,
                0);

        CHECK(control != NULL);
        CHECK(control->isValid());

        SurfaceComposerClient::openGlobalTransaction();
        
        control->setPosition(100, 100);
        control->setSize(1000, 800);
        
        CHECK_EQ(control->setLayer(INT_MAX), (status_t)OK);
        CHECK_EQ(control->show(), (status_t)OK);
        SurfaceComposerClient::closeGlobalTransaction();

        surface = control->getSurface();
        CHECK(surface != NULL);
    }

    if (playback) {
        printf("2. ########\n");
        sp<SimplePlayer> player = new SimplePlayer;
        looper->registerHandler(player);

        player->setDataSource(argv[0]);
        player->setSurface(surface->getIGraphicBufferProducer());
        player->start();
        sleep(60);
        player->stop();
        player->reset();
    } else {
        printf("3. ########\n");
        decode(looper, argv[0], useAudio, useVideo, surface, renderSurface,
                useTimestamp);
    }

    if (playback || (useSurface && useVideo)) {
        printf("4. ########\n");
        composerClient->dispose();
    }

    looper->stop();

    return 0;
}

