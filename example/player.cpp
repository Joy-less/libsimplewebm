/*
 *    MIT License
 *
 *    Copyright (c) 2016 Błażej Szczygieł
 * 	  Copyright (c) 2026-present The Simplicity Group & contributors
 * 	  See AUTHORS.md for more information
 *
 *    Permission is hereby granted, free of charge, to any person obtaining a copy
 *    of this software and associated documentation files (the "Software"), to deal
 *    in the Software without restriction, including without limitation the rights
 *    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *    copies of the Software, and to permit persons to whom the Software is
 *    furnished to do so, subject to the following conditions:
 *
 *    The above copyright notice and this permission notice shall be included in all
 *    copies or substantial portions of the Software.
 *
 *    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *    SOFTWARE.
 */

#include "include/decoder/OpusDecoder.h"
#include "include/decoder/VorbisDecoder.h"
#include "include/decoder/VPXDecoder.h"

#include <SDL3/SDL.h>
#include <webm/file_reader.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <vector>

struct DecodedVideoFrame {
    double time = 0.0;
    int width = 0;
    int height = 0;
    int chromaWidth = 0;
    int chromaHeight = 0;

    std::vector<unsigned char> y;
    std::vector<unsigned char> u;
    std::vector<unsigned char> v;
};

static bool copyPlane(std::vector<unsigned char> &destination, const unsigned char *source, int sourceStride, int width, int height)
{
    if (!source || width <= 0 || height <= 0 || sourceStride < width)
        return false;

    const size_t rowWidth = static_cast<size_t>(width);
    const size_t rows = static_cast<size_t>(height);

    if (rowWidth > std::numeric_limits<size_t>::max() / rows)
        return false;

    destination.resize(rowWidth * rows);

    for (int row = 0; row < height; ++row) {
        std::memcpy(destination.data() + static_cast<size_t>(row) * rowWidth, source + static_cast<size_t>(row) * sourceStride, rowWidth);
    }

    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <file.webm>\n", argv[0]);
        return 1;
    }

    FILE *file = std::fopen(argv[1], "rb");
    if (!file) {
        std::fprintf(stderr, "Failed to open: %s\n", argv[1]);
        return 1;
    }

    webm::FileReader reader(file);
    WebMDemuxer demuxer(&reader);

    if (!demuxer.isOpen()) {
        std::fprintf(stderr, "Failed to open WebM demuxer.\n");
        std::fclose(file);
        return 1;
    }

    if (demuxer.getVideoCodec() == WebMDemuxer::NO_VIDEO) {
        std::fprintf(stderr, "The WebM file has no supported video stream.\n");
        std::fclose(file);
        return 1;
    }

    VPXDecoder videoDecoder(demuxer, 1);

    if (!videoDecoder.isOpen()) {
        std::fprintf(stderr, "Failed to initialize the video decoder.\n");
        std::fclose(file);
        return 1;
    }

    const bool hasAudio = demuxer.getAudioCodec() != WebMDemuxer::NO_AUDIO;

    std::unique_ptr<OpusDecoder> opusDecoder;
    std::unique_ptr<VorbisDecoder> vorbisDecoder;

    int channels = 0;
    int sampleRate = 0;

    if (hasAudio) {
        channels = demuxer.getChannels();
        sampleRate = static_cast<int>(demuxer.getSampleRate());

        if (channels <= 0 || sampleRate <= 0) {
            std::fprintf(stderr, "Invalid audio stream format.\n");
            std::fclose(file);
            return 1;
        }

        if (demuxer.getAudioCodec() == WebMDemuxer::AUDIO_OPUS) {
            opusDecoder = std::make_unique<OpusDecoder>(demuxer);

            if (!opusDecoder->isOpen()) {
                std::fprintf(stderr, "Failed to initialize the Opus decoder.\n");
                std::fclose(file);
                return 1;
            }
        } else if (demuxer.getAudioCodec() == WebMDemuxer::AUDIO_VORBIS) {
            vorbisDecoder = std::make_unique<VorbisDecoder>(demuxer);

            if (!vorbisDecoder->isOpen()) {
                std::fprintf(stderr, "Failed to initialize the Vorbis decoder.\n");
                std::fclose(file);
                return 1;
            }
        } else {
            std::fprintf(stderr, "Unsupported audio codec.\n");
            std::fclose(file);
            return 1;
        }
    }

    std::unique_ptr<short[]> pcm;
    int pcmCapacity = 0;

    if (hasAudio) {
        const int bufferSamples = opusDecoder ? opusDecoder->getBufferSamples() : vorbisDecoder->getBufferSamples();

        if (bufferSamples <= 0 || bufferSamples > std::numeric_limits<int>::max() / channels) {
            std::fprintf(stderr, "Invalid audio decoder buffer size.\n");
            std::fclose(file);
            return 1;
        }

        pcmCapacity = bufferSamples * channels;
        pcm = std::make_unique<short[]>(pcmCapacity);
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        std::fclose(file);
        return 1;
    }

    SDL_AudioStream *audioStream = nullptr;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;

    bool success = true;
    bool running = true;
    bool audioStarted = false;
    bool demuxerEOF = false;
    bool audioDrainStarted = false;

    double audioDrainWallTime = 0.0;
    double playbackStart = SDL_GetTicks() / 1000.0;

    unsigned long long submittedSamples = 0;

    WebMFrame videoFrame;
    WebMFrame audioFrame;
    VPXDecoder::Image image;

    std::deque<DecodedVideoFrame> videoQueue;

    constexpr double audioPrebufferSeconds = 0.25;
    constexpr double audioDrainGraceSeconds = 0.15;
    constexpr size_t maxSilentVideoQueue = 4;

    const double bytesPerSecond = hasAudio ? static_cast<double>(sampleRate) * channels * sizeof(short) : 0.0;

    if (hasAudio) {
        SDL_AudioSpec audioSpec{};
        audioSpec.format = SDL_AUDIO_S16;
        audioSpec.channels = channels;
        audioSpec.freq = sampleRate;

        audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audioSpec, nullptr, nullptr);

        if (!audioStream) {
            std::fprintf(stderr, "Failed to open audio device: %s\n", SDL_GetError());
            success = false;
            running = false;
        }
    }

    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                running = false;
        }

        if (!running)
            break;

        const double now = SDL_GetTicks() / 1000.0;

        int queuedBytes = 0;

        if (hasAudio && audioStream) {
            queuedBytes = SDL_GetAudioStreamQueued(audioStream);

            if (queuedBytes < 0) {
                std::fprintf(stderr, "Failed to query audio queue: %s\n", SDL_GetError());
                success = false;
                break;
            }
        }

        double playbackTime = now - playbackStart;

        if (hasAudio) {
            if (!audioStarted) {
                // Don't let video run ahead before audio playback begins
                playbackTime = 0.0;
            } else {
                const double audioClock = static_cast<double>(submittedSamples) / sampleRate - static_cast<double>(queuedBytes) / bytesPerSecond;

                playbackTime = std::max(0.0, audioClock);

                if (demuxerEOF && queuedBytes == 0) {
                    if (!audioDrainStarted) {
                        audioDrainStarted = true;
                        audioDrainWallTime = now;
                    }

                    // Keep the clock advancing after the final queued samples have left SDL's stream queue
                    playbackTime = static_cast<double>(submittedSamples) / sampleRate + (now - audioDrainWallTime);
                }
            }
        }

        // Read ahead to keep audio fed, but don't decode an entire silent video into memory when there is no audio track
        bool shouldReadMore = !demuxerEOF;

        if (shouldReadMore) {
            if (hasAudio) {
                const double queuedSeconds = static_cast<double>(queuedBytes) / bytesPerSecond;

                shouldReadMore = !audioStarted || queuedSeconds < audioPrebufferSeconds || videoQueue.empty();
            } else {
                shouldReadMore = videoQueue.size() < maxSilentVideoQueue;
            }
        }

        if (shouldReadMore) {
            if (!demuxer.readFrame(&videoFrame, hasAudio ? &audioFrame : nullptr)) {
                demuxerEOF = true;
            } else {
                if (videoFrame.isValid()) {
                    videoDecoder.decode(videoFrame);

                    const VPXDecoder::IMAGE_ERROR imageError = videoDecoder.getImage(image);

                    if (imageError == VPXDecoder::NO_ERROR) {
                        // SDL_PIXELFORMAT_IYUV expects 4:2:0 chroma
                        if (image.chromaShiftW != 1 || image.chromaShiftH != 1) {
                            std::fprintf(stderr, "Unsupported video chroma subsampling.\n");
                            success = false;
                            running = false;
                            continue;
                        }

                        DecodedVideoFrame decoded;
                        decoded.time = videoFrame.time;
                        decoded.width = image.w;
                        decoded.height = image.h;
                        decoded.chromaWidth = image.getWidth(1);
                        decoded.chromaHeight = image.getHeight(1);

                        // Copy all planes before the next decoder call
                        // libvpx owns the original plane memory
                        if (!copyPlane(decoded.y, image.planes[0], image.linesize[0], decoded.width, decoded.height) ||
                            !copyPlane(decoded.u, image.planes[1], image.linesize[1], decoded.chromaWidth, decoded.chromaHeight) ||
						    !copyPlane(decoded.v, image.planes[2], image.linesize[2], decoded.chromaWidth, decoded.chromaHeight)) 
						{
                            std::fprintf(stderr, "Failed to copy decoded video frame.\n");
                            success = false;
                            running = false;
                            continue;
                        }

                        videoQueue.push_back(std::move(decoded));
                    } else if (imageError != VPXDecoder::NO_FRAME) {
                        std::fprintf(stderr, "Failed to decode video frame.\n");
                        success = false;
                        running = false;
                        continue;
                    }
                }

                if (hasAudio && audioFrame.isValid()) {
                    int numOutSamples = 0;

                    const bool decoded = opusDecoder ? opusDecoder->getPCMS16(audioFrame, pcm.get(), pcmCapacity, numOutSamples) : vorbisDecoder->getPCMS16(audioFrame, pcm.get(), pcmCapacity, numOutSamples);

                    if (!decoded) {
                        std::fprintf(stderr, "Failed to decode audio packet.\n");
                        success = false;
                        running = false;
                        continue;
                    }

                    if (numOutSamples > 0) {
                        const size_t sampleCount = static_cast<size_t>(numOutSamples) * channels;

                        if (sampleCount >
                            std::numeric_limits<size_t>::max() / sizeof(short)) {
                            std::fprintf(stderr, "Audio packet size overflow.\n");
                            success = false;
                            running = false;
                            continue;
                        }

                        const size_t pcmBytes = sampleCount * sizeof(short);

                        if (pcmBytes >
                            static_cast<size_t>(std::numeric_limits<int>::max())) {
                            std::fprintf(stderr, "Audio packet is too large.\n");
                            success = false;
                            running = false;
                            continue;
                        }

                        if (!SDL_PutAudioStreamData(audioStream, pcm.get(), static_cast<int>(pcmBytes))) {
                            std::fprintf(stderr, "Failed to queue audio: %s\n", SDL_GetError());
                            success = false;
                            running = false;
                            continue;
                        }

                        submittedSamples += static_cast<unsigned long long>(numOutSamples);

                        if (!audioStarted) {
                            audioStarted = true;

                            if (!SDL_ResumeAudioStreamDevice(audioStream)) {
                                std::fprintf(stderr, "Failed to start audio playback: %s\n", SDL_GetError());
                                success = false;
                                running = false;
                                continue;
                            }
                        }
                    }
                }
            }
        }

        if (!running)
            break;

        // Refresh the clock after adding any newly decoded audio
        const double frameNow = SDL_GetTicks() / 1000.0;

        if (hasAudio && audioStream && audioStarted) {
            queuedBytes = SDL_GetAudioStreamQueued(audioStream);

            if (queuedBytes < 0) {
                std::fprintf(stderr, "Failed to query audio queue: %s\n", SDL_GetError());
                success = false;
                break;
            }

            playbackTime = std::max(0.0, static_cast<double>(submittedSamples) / sampleRate - static_cast<double>(queuedBytes) / bytesPerSecond);

            if (demuxerEOF && queuedBytes == 0) {
                if (!audioDrainStarted) {
                    audioDrainStarted = true;
                    audioDrainWallTime = frameNow;
                }

                playbackTime = static_cast<double>(submittedSamples) / sampleRate + (frameNow - audioDrainWallTime);
            }
        } else if (!hasAudio) {
            playbackTime = frameNow - playbackStart;
        }

        // Present every frame at its timestamp, using owned plane memory
        if (!videoQueue.empty() &&
            videoQueue.front().time <= playbackTime) {
            DecodedVideoFrame &frame = videoQueue.front();

            if (!window) {
                window = SDL_CreateWindow("libsimplewebm player", frame.width, frame.height, SDL_WINDOW_RESIZABLE);

                if (window)
                    renderer = SDL_CreateRenderer(window, nullptr);

                if (!window || !renderer) {
                    std::fprintf(stderr, "Failed to create video window: %s\n", SDL_GetError());
                    success = false;
                    break;
                }
            }

            if (!texture || SDL_GetTextureSize(texture, nullptr, nullptr) == false) {
                if (texture) {
                    SDL_DestroyTexture(texture);
                    texture = nullptr;
                }

                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
            }

            if (!texture) {
                std::fprintf(stderr, "Failed to create video texture: %s\n", SDL_GetError());
                success = false;
                break;
            }

            if (!SDL_UpdateYUVTexture(texture, nullptr, frame.y.data(), frame.width, frame.u.data(), frame.chromaWidth, frame.v.data(), frame.chromaWidth)) {
                std::fprintf(stderr, "Failed to update video texture: %s\n", SDL_GetError());
                success = false;
                break;
            }

            SDL_RenderClear(renderer);
            SDL_RenderTexture(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);

            videoQueue.pop_front();
        }

        // EOF means no more packets, not that playback has finished
        const bool audioFinished = !hasAudio || !audioStarted || (demuxerEOF && queuedBytes == 0 && audioDrainStarted && frameNow - audioDrainWallTime >= audioDrainGraceSeconds);

        if (demuxerEOF && videoQueue.empty() && audioFinished)
            break;

        SDL_Delay(1);
    }

    if (audioStream)
        SDL_DestroyAudioStream(audioStream);

    if (texture)
        SDL_DestroyTexture(texture);

    if (renderer)
        SDL_DestroyRenderer(renderer);

    if (window)
        SDL_DestroyWindow(window);

    SDL_Quit();

    return success ? 0 : 1;
}