﻿// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2011-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#include "rgy_output.h"
#include "rgy_filesystem.h"
#include "rgy_bitstream.h"
#include "rgy_language.h"
#include <filesystem>
#if defined(_M_IX86) || defined(_M_X64) || defined(__x86_64)
#include <smmintrin.h>
#endif

#if ENCODER_QSV

static RGY_ERR WriteY4MHeader(FILE *fp, const VideoInfo *info) {
    char buffer[256] = { 0 };
    char *ptr = buffer;
    uint32_t len = 0;
    memcpy(ptr, "YUV4MPEG2 ", 10);
    len += 10;

    len += sprintf_s(ptr+len, sizeof(buffer)-len, "W%d H%d ", info->dstWidth, info->dstHeight);
    len += sprintf_s(ptr+len, sizeof(buffer)-len, "F%d:%d ", info->fpsN, info->fpsD);

    const char *picstruct = "Ip ";
    if (info->picstruct & RGY_PICSTRUCT_TFF) {
        picstruct = "It ";
    } else if (info->picstruct & RGY_PICSTRUCT_BFF) {
        picstruct = "Ib ";
    }
    strcpy_s(ptr+len, sizeof(buffer)-len, picstruct); len += 3;
    len += sprintf_s(ptr+len, sizeof(buffer)-len, "A%d:%d ", info->sar[0], info->sar[1]);
    strcpy_s(ptr+len, sizeof(buffer)-len, "C420mpeg2\n"); len += (mfxU32)strlen("C420mpeg2\n");
    return (len == fwrite(buffer, 1, len, fp)) ? RGY_ERR_NONE : RGY_ERR_UNDEFINED_BEHAVIOR;
}

#endif //#if ENCODER_QSV

#define WRITE_CHECK(writtenBytes, expected) { \
    if (writtenBytes != expected) { \
        AddMessage(RGY_LOG_ERROR, _T("Error writing file.\nNot enough disk space!\n")); \
        return RGY_ERR_UNDEFINED_BEHAVIOR; \
    } }

const char *RGYOutput::OUT_DEBUG_FILE_HEADER = "size %d, pts %lld, dts %lld, duration %lld, frametype %d, frameidx %d, picstruct %d";

RGYOutput::RGYOutput() :
    m_outFilename(),
    m_encSatusInfo(),
    m_fDest(),
    m_fpDebug(),
    m_fpOutReplay(),
    m_outputIsStdout(false),
    m_inited(false),
    m_noOutput(false),
    m_OutType(OUT_TYPE_BITSTREAM),
    m_sourceHWMem(false),
    m_y4mHeaderWritten(false),
    m_strWriterName(),
    m_strOutputInfo(),
    m_VideoOutputInfo(),
    m_printMes(),
    m_outputBuffer(),
    m_readBuffer(),
    m_UVBuffer() {
}

RGYOutput::~RGYOutput() {
    m_encSatusInfo.reset();
    m_printMes.reset();
    Close();
}

void RGYOutput::Close() {
    AddMessage(RGY_LOG_DEBUG, _T("Closing file \"%s\"...\n"), m_outFilename.c_str());
    if (m_fDest) {
        m_fDest.reset();
        AddMessage(RGY_LOG_DEBUG, _T("Closed file pointer.\n"));
    }
    m_fpOutReplay.reset();
    m_fpDebug.reset();
    m_encSatusInfo.reset();
    m_outputBuffer.reset();
    m_readBuffer.reset();
    m_UVBuffer.reset();

    m_noOutput = false;
    m_inited = false;
    m_sourceHWMem = false;
    m_y4mHeaderWritten = false;
    AddMessage(RGY_LOG_DEBUG, _T("Closed.\n"));
    m_printMes.reset();
}

RGY_ERR RGYOutput::writeRawDebug(RGYBitstream *pBitstream) {
    if (!m_fpDebug) return RGY_ERR_NONE;

    char frame_info[256] = { 0 };
    sprintf_s(frame_info, OUT_DEBUG_FILE_HEADER,
        (int)pBitstream->size(), pBitstream->pts(), pBitstream->dts(), pBitstream->duration(),
        pBitstream->frametype(), pBitstream->frameIdx(), pBitstream->picstruct());
    _fwrite_nolock(frame_info, 1, sizeof(frame_info), m_fpDebug.get());
    _fwrite_nolock(pBitstream->data(), 1, pBitstream->size(), m_fpDebug.get());
    return RGY_ERR_NONE;
}

RGY_ERR RGYOutput::readRawDebug(RGYBitstream *pBitstream) {
    if (!m_fpOutReplay) return RGY_ERR_NONE;

    char frame_info[256] = { 0 };
    if (_fread_nolock(frame_info, 1, sizeof(frame_info), m_fpOutReplay.get()) != sizeof(frame_info)) {
        return RGY_ERR_MORE_DATA;
    }
    int size = 0, frameIdx = 0;
    int64_t pts = 0, dts = 0, duration = 0;
    RGY_FRAMETYPE frametype = RGY_FRAMETYPE_UNKNOWN;
    RGY_PICSTRUCT picstruct = RGY_PICSTRUCT_UNKNOWN;
    if (sscanf_s(frame_info, OUT_DEBUG_FILE_HEADER, &size, &pts, &dts, &duration, &frametype, &frameIdx, &picstruct) != 7) {
        return RGY_ERR_INVALID_DATA_TYPE;
    }
    std::vector<uint8_t> buffer(size, 0);
    if (_fread_nolock(buffer.data(), 1, buffer.size(), m_fpOutReplay.get()) != buffer.size()) {
        return RGY_ERR_MORE_DATA;
    }
    pBitstream->setDuration(duration);
    pBitstream->setFrameIdx(frameIdx);
    pBitstream->setFrametype(frametype);
    pBitstream->setPicstruct(picstruct);
    pBitstream->copy(buffer.data(), buffer.size(), dts, pts);
    return RGY_ERR_NONE;
}

RGYOutputRaw::RGYOutputRaw() :
    m_outputBuf2(),
    m_hdrBitstream(),
    m_doviRpu(nullptr),
    m_timestamp(nullptr),
    m_prevInputFrameId(-1),
    m_prevEncodeFrameId(-1),
    m_debugDirectAV1Out(false),
#if ENABLE_AVSW_READER
    m_pBsfc(),
    m_pkt(),
#endif //#if ENABLE_AVSW_READER
    parse_nal_h264(get_parse_nal_unit_h264_func()),
    parse_nal_hevc(get_parse_nal_unit_hevc_func()) {
    m_strWriterName = _T("bitstream");
    m_OutType = OUT_TYPE_BITSTREAM;
}

RGYOutputRaw::~RGYOutputRaw() {
    if (m_fpDebug) {
        m_fpDebug.reset();
    }
#if ENABLE_AVSW_READER
    m_pBsfc.reset();
    m_pkt.reset();
#endif //#if ENABLE_AVSW_READER
}

#pragma warning (push)
#pragma warning (disable: 4127) //warning C4127: 条件式が定数です。
RGY_ERR RGYOutputRaw::Init(const TCHAR *strFileName, const VideoInfo *pVideoOutputInfo, const void *prm) {
    UNREFERENCED_PARAMETER(pVideoOutputInfo);
    RGYOutputRawPrm *rawPrm = (RGYOutputRawPrm *)prm;
    if (!rawPrm->benchmark && _tcslen(strFileName) == 0) {
        AddMessage(RGY_LOG_ERROR, _T("output filename not set.\n"));
        return RGY_ERR_INVALID_PARAM;
    }

    if (rawPrm->benchmark) {
        m_noOutput = true;
        AddMessage(RGY_LOG_DEBUG, _T("no output for benchmark mode.\n"));
    } else {
        if (_tcscmp(strFileName, _T("-")) == 0) {
            m_fDest.reset(stdout);
            m_outputIsStdout = true;
            AddMessage(RGY_LOG_DEBUG, _T("using stdout\n"));
        } else {
            CreateDirectoryRecursive(PathRemoveFileSpecFixed(strFileName).second.c_str());
            FILE *fp = NULL;
            int error = _tfopen_s(&fp, strFileName, _T("wb+"));
            if (error != 0 || fp == NULL) {
                AddMessage(RGY_LOG_ERROR, _T("failed to open output file \"%s\": %s\n"), strFileName, _tcserror(error));
                return RGY_ERR_FILE_OPEN;
            }
            m_fDest.reset(fp);
            AddMessage(RGY_LOG_DEBUG, _T("Opened file \"%s\"\n"), strFileName);

            int bufferSizeByte = clamp(rawPrm->bufSizeMB, 0, RGY_OUTPUT_BUF_MB_MAX) * 1024 * 1024;
            if (bufferSizeByte) {
                void *ptr = nullptr;
                bufferSizeByte = (int)malloc_degeneracy(&ptr, bufferSizeByte, 1024 * 1024);
                if (bufferSizeByte) {
                    m_outputBuffer.reset((char*)ptr);
                    setvbuf(m_fDest.get(), m_outputBuffer.get(), _IOFBF, bufferSizeByte);
                    AddMessage(RGY_LOG_DEBUG, _T("Added %d MB output buffer.\n"), bufferSizeByte / (1024 * 1024));
                }
            }
        }
#if ENABLE_AVSW_READER
        if ((ENCODER_NVENC
            && (pVideoOutputInfo->codec == RGY_CODEC_H264 || pVideoOutputInfo->codec == RGY_CODEC_HEVC)
            && pVideoOutputInfo->sar[0] * pVideoOutputInfo->sar[1] > 0)
            || (ENCODER_QSV
                && (pVideoOutputInfo->codec == RGY_CODEC_H264 || pVideoOutputInfo->codec == RGY_CODEC_HEVC)
                && pVideoOutputInfo->vui.chromaloc != 0)
            || (ENCODER_VCEENC
                && (pVideoOutputInfo->codec == RGY_CODEC_HEVC // HEVCの時は常に上書き
                    || (pVideoOutputInfo->vui.format != 5
                    || pVideoOutputInfo->vui.colorprim != 2
                    || pVideoOutputInfo->vui.transfer != 2
                    || pVideoOutputInfo->vui.matrix != 2
                    || pVideoOutputInfo->vui.chromaloc != 0)))
            || (ENCODER_MPP
                && ((pVideoOutputInfo->codec == RGY_CODEC_H264 || pVideoOutputInfo->codec == RGY_CODEC_HEVC) // HEVCの時は常に上書き)
                    || (pVideoOutputInfo->sar[0] * pVideoOutputInfo->sar[1] > 0
                    || (pVideoOutputInfo->vui.format != 5
                        || pVideoOutputInfo->vui.colorprim != 2
                        || pVideoOutputInfo->vui.transfer != 2
                        || pVideoOutputInfo->vui.matrix != 2
                        || pVideoOutputInfo->vui.chromaloc != 0))))) {
            if (!check_avcodec_dll()) {
                AddMessage(RGY_LOG_ERROR, error_mes_avcodec_dll_not_found());
                return RGY_ERR_NULL_PTR;
            }

            const char *bsf_name = nullptr;
            switch (pVideoOutputInfo->codec) {
            case RGY_CODEC_H264: bsf_name = "h264_metadata"; break;
            case RGY_CODEC_HEVC: bsf_name = "hevc_metadata"; break;
            case RGY_CODEC_AV1:  bsf_name = "av1_metadata"; break;
            default:
                break;
            }
            if (bsf_name == nullptr) {
                AddMessage(RGY_LOG_ERROR, _T("invalid codec to set metadata filter.\n"));
                return RGY_ERR_INVALID_CALL;
            }
            const auto bsf_tname = char_to_tstring(bsf_name);
            AddMessage(RGY_LOG_DEBUG, _T("start initialize %s filter...\n"), bsf_tname.c_str());
            auto filter = av_bsf_get_by_name(bsf_name);
            if (filter == nullptr) {
                AddMessage(RGY_LOG_ERROR, _T("failed to find %s.\n"), bsf_tname.c_str());
                return RGY_ERR_NOT_FOUND;
            }
            unique_ptr<AVCodecParameters, RGYAVDeleter<AVCodecParameters>> codecpar(avcodec_parameters_alloc(), RGYAVDeleter<AVCodecParameters>(avcodec_parameters_free));

            codecpar->codec_type              = AVMEDIA_TYPE_VIDEO;
            codecpar->codec_id                = getAVCodecId(pVideoOutputInfo->codec);
            codecpar->width                   = pVideoOutputInfo->dstWidth;
            codecpar->height                  = pVideoOutputInfo->dstHeight;
            codecpar->format                  = csp_rgy_to_avpixfmt(pVideoOutputInfo->csp);
            codecpar->level                   = pVideoOutputInfo->codecLevel;
            codecpar->profile                 = pVideoOutputInfo->codecProfile;
            codecpar->sample_aspect_ratio.num = pVideoOutputInfo->sar[0];
            codecpar->sample_aspect_ratio.den = pVideoOutputInfo->sar[1];
            codecpar->chroma_location         = (AVChromaLocation)pVideoOutputInfo->vui.chromaloc;
            codecpar->field_order             = picstrcut_rgy_to_avfieldorder(pVideoOutputInfo->picstruct);
            codecpar->video_delay             = pVideoOutputInfo->videoDelay;
            if (pVideoOutputInfo->vui.descriptpresent) {
                codecpar->color_space         = (AVColorSpace)pVideoOutputInfo->vui.matrix;
                codecpar->color_primaries     = (AVColorPrimaries)pVideoOutputInfo->vui.colorprim;
                codecpar->color_range         = (AVColorRange)pVideoOutputInfo->vui.colorrange;
                codecpar->color_trc           = (AVColorTransferCharacteristic)pVideoOutputInfo->vui.transfer;
            }
            int ret = 0;
            AVBSFContext *bsfc = nullptr;
            if (0 > (ret = av_bsf_alloc(filter, &bsfc))) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for %s: %s.\n"), bsf_tname.c_str(), qsv_av_err2str(ret).c_str());
                return RGY_ERR_NULL_PTR;
            }
            if (0 > (ret = avcodec_parameters_copy(bsfc->par_in, codecpar.get()))) {
                AddMessage(RGY_LOG_ERROR, _T("failed to copy parameter for %s: %s.\n"), bsf_tname.c_str(), qsv_av_err2str(ret).c_str());
                return RGY_ERR_UNKNOWN;
            }
            m_pBsfc = unique_ptr<AVBSFContext, RGYAVDeleter<AVBSFContext>>(bsfc, RGYAVDeleter<AVBSFContext>(av_bsf_free));
            AVDictionary *bsfPrm = nullptr;
            if (ENCODER_MPP) {
                const auto level_str = get_cx_desc(get_level_list(pVideoOutputInfo->codec), pVideoOutputInfo->codecLevel);
                av_dict_set(&bsfPrm, "level", tchar_to_string(level_str).c_str(), 0);
                AddMessage(RGY_LOG_DEBUG, _T("set level %s by %s filter\n"), level_str, bsf_tname.c_str());
            }
            if ((ENCODER_NVENC || ENCODER_MPP) && pVideoOutputInfo->sar[0] * pVideoOutputInfo->sar[1] > 0) {
                char sar[128];
                sprintf_s(sar, "%d/%d", pVideoOutputInfo->sar[0], pVideoOutputInfo->sar[1]);
                av_dict_set(&bsfPrm, "sample_aspect_ratio", sar, 0);
                AddMessage(RGY_LOG_DEBUG, _T("set sar %d:%d by %s filter\n"), pVideoOutputInfo->sar[0], pVideoOutputInfo->sar[1], bsf_tname.c_str());
            }
            if (ENCODER_VCEENC) {
                // HEVCの10bitの時、エンコーダがおかしなVUIを設定することがあるのでこれを常に上書き
                const bool override_always = pVideoOutputInfo->codec == RGY_CODEC_HEVC;
                if (override_always || pVideoOutputInfo->vui.format != 5 /*undef*/) {
                    av_dict_set_int(&bsfPrm, "video_format", pVideoOutputInfo->vui.format, 0);
                    AddMessage(RGY_LOG_DEBUG, _T("set video_format %d by %s filter\n"), pVideoOutputInfo->vui.format, bsf_tname.c_str());
                }
                if (override_always || pVideoOutputInfo->vui.colorprim != 2 /*undef*/) {
                    av_dict_set_int(&bsfPrm, "colour_primaries", pVideoOutputInfo->vui.colorprim, 0);
                    AddMessage(RGY_LOG_DEBUG, _T("set colorprim %d by %s filter\n"), pVideoOutputInfo->vui.colorprim, bsf_tname.c_str());
                }
                if (override_always || pVideoOutputInfo->vui.transfer != 2 /*undef*/) {
                    av_dict_set_int(&bsfPrm, "transfer_characteristics", pVideoOutputInfo->vui.transfer, 0);
                    AddMessage(RGY_LOG_DEBUG, _T("set transfer %d by %s filter\n"), pVideoOutputInfo->vui.transfer, bsf_tname.c_str());
                }
                if (override_always || pVideoOutputInfo->vui.matrix != 2 /*undef*/) {
                    av_dict_set_int(&bsfPrm, "matrix_coefficients", pVideoOutputInfo->vui.matrix, 0);
                    AddMessage(RGY_LOG_DEBUG, _T("set matrix %d by %s filter\n"), pVideoOutputInfo->vui.matrix, bsf_tname.c_str());
                }
            }
            if (ENCODER_QSV || ENCODER_VCEENC) {
                if (pVideoOutputInfo->vui.chromaloc != 0) {
                    av_dict_set_int(&bsfPrm, "chroma_sample_loc_type", pVideoOutputInfo->vui.chromaloc-1, 0);
                    AddMessage(RGY_LOG_DEBUG, _T("set chromaloc %d by %s filter\n"), pVideoOutputInfo->vui.chromaloc-1, bsf_tname.c_str());
                }
            }
            if (0 > (ret = av_opt_set_dict2(m_pBsfc.get(), &bsfPrm, AV_OPT_SEARCH_CHILDREN))) {
                AddMessage(RGY_LOG_ERROR, _T("failed to set parameters for %s: %s.\n"), bsf_tname.c_str(), qsv_av_err2str(ret).c_str());
                return RGY_ERR_UNKNOWN;
            }
            if (0 > (ret = av_bsf_init(m_pBsfc.get()))) {
                AddMessage(RGY_LOG_ERROR, _T("failed to init %s: %s.\n"), bsf_tname.c_str(), qsv_av_err2str(ret).c_str());
                return RGY_ERR_UNKNOWN;
            }
            AddMessage(RGY_LOG_DEBUG, _T("initialized %s filter\n"), bsf_tname.c_str());

            m_pkt = std::unique_ptr<AVPacket, RGYAVDeleter<AVPacket>>(av_packet_alloc(), RGYAVDeleter<AVPacket>(av_packet_free));
        }
#endif //#if ENABLE_AVSW_READER
        if (rawPrm->hdrMetadata != nullptr && rawPrm->hdrMetadata->getprm().hasPrmSet()) {
            AddMessage(RGY_LOG_DEBUG, char_to_tstring(rawPrm->hdrMetadata->print()));
            if (rawPrm->codecId == RGY_CODEC_HEVC) {
                m_hdrBitstream = rawPrm->hdrMetadata->gen_nal();
            } else if (rawPrm->codecId == RGY_CODEC_AV1) {
                m_hdrBitstream = rawPrm->hdrMetadata->gen_obu();
            } else {
                AddMessage(RGY_LOG_ERROR, _T("Setting masterdisplay/contentlight not supported in %s encoding.\n"), CodecToStr(rawPrm->codecId).c_str());
                return RGY_ERR_UNSUPPORTED;
            }
        }
        m_doviRpu = rawPrm->doviRpu;
        m_timestamp = rawPrm->vidTimestamp;
        m_debugDirectAV1Out = rawPrm->debugDirectAV1Out;
        if (rawPrm->debugRawOut) {
            const auto filename_debug = m_outFilename + _T(".debug");
            m_fpDebug = std::unique_ptr<FILE, fp_deleter>(
                _tfopen(filename_debug.c_str(), _T("wb")), fp_deleter());
            if (!m_fpDebug) {
                AddMessage(RGY_LOG_ERROR, _T("Failed to open raw frame debug out file \"%s\".\n"), filename_debug.c_str());
                return RGY_ERR_FILE_OPEN;
            }
            AddMessage(RGY_LOG_INFO, _T("Raw frame debug out to file \"%s\".\n"), filename_debug.c_str());
        }
        if (!rawPrm->outReplayFile.empty()) {
            m_fpOutReplay = std::unique_ptr<FILE, fp_deleter>(
                _tfopen(rawPrm->outReplayFile.c_str(), _T("rb")), fp_deleter());
            if (!m_fpOutReplay) {
                AddMessage(RGY_LOG_ERROR, _T("Failed to open replay debug out from file \"%s\".\n"), rawPrm->outReplayFile.c_str());
                return RGY_ERR_FILE_OPEN;
            }

            AddMessage(RGY_LOG_WARN, _T("replay debug out from file \"%s\".\n"), rawPrm->outReplayFile.c_str());
            if (rawPrm->outReplayCodec != RGY_CODEC_UNKNOWN) {
                m_VideoOutputInfo.codec = rawPrm->outReplayCodec;
                AddMessage(RGY_LOG_WARN, _T("replay codec set to \"%s\".\n"), CodecToStr(m_VideoOutputInfo.codec).c_str());
            }
        }
    }
    m_inited = true;
    return RGY_ERR_NONE;
}
#pragma warning (pop)

RGY_ERR RGYOutputRaw::WriteNextFrame(RGYBitstream *pBitstream) {
    if (pBitstream == nullptr) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid call: WriteNextFrame\n"));
        return RGY_ERR_NULL_PTR;
    }

    readRawDebug(pBitstream);

    size_t nBytesWritten = 0;
    if (!m_noOutput) {
#if ENABLE_AVSW_READER
        if (m_pBsfc) {
            uint8_t nal_type = 0;
            std::vector<nal_info> nal_list;
            if (m_VideoOutputInfo.codec == RGY_CODEC_HEVC) {
                nal_type = NALU_HEVC_SPS;
                nal_list = parse_nal_hevc(pBitstream->data(), pBitstream->size());
            } else if (m_VideoOutputInfo.codec == RGY_CODEC_H264) {
                nal_type = NALU_H264_SPS;
                nal_list = parse_nal_h264(pBitstream->data(), pBitstream->size());
            }
            auto sps_nal = std::find_if(nal_list.begin(), nal_list.end(), [nal_type](nal_info info) { return info.type == nal_type; });
            if (sps_nal != nal_list.end()) {
                AVPacket *pkt = m_pkt.get();
                av_new_packet(pkt, (int)sps_nal->size);
                memcpy(pkt->data, sps_nal->ptr, sps_nal->size);
                int ret = 0;
                if (0 > (ret = av_bsf_send_packet(m_pBsfc.get(), pkt))) {
                    av_packet_unref(pkt);
                    AddMessage(RGY_LOG_ERROR, _T("failed to send packet to %s bitstream filter: %s.\n"),
                        char_to_tstring(m_pBsfc->filter->name).c_str(), qsv_av_err2str(ret).c_str());
                    return RGY_ERR_UNKNOWN;
                }
                ret = av_bsf_receive_packet(m_pBsfc.get(), pkt);
                if (ret == AVERROR(EAGAIN)) {
                    return RGY_ERR_NONE;
                } else if ((ret < 0 && ret != AVERROR_EOF) || pkt->size < 0) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to run %s bitstream filter: %s.\n"),
                        char_to_tstring(m_pBsfc->filter->name).c_str(), qsv_av_err2str(ret).c_str());
                    return RGY_ERR_UNKNOWN;
                }
                const auto new_data_size = pBitstream->size() + pkt->size - sps_nal->size;
                const auto sps_nal_offset = sps_nal->ptr - pBitstream->data();
                const auto next_nal_orig_offset = sps_nal_offset + sps_nal->size;
                const auto next_nal_new_offset = sps_nal_offset + pkt->size;
                const auto stream_orig_length = pBitstream->size();
                if (pBitstream->size() < new_data_size
                    && (decltype(new_data_size))pBitstream->bufsize() < new_data_size) {
#if ENCODER_QSV
                    pBitstream->changeSize(new_data_size);
#else //NVEnc, VCEの場合はこうしないとメモリリークが発生する
                    const auto org_data_size = pBitstream->size();
                    m_outputBuf2.resize(std::max(new_data_size, org_data_size));
                    memcpy(m_outputBuf2.data(), pBitstream->data(), org_data_size);
                    pBitstream->release();
                    pBitstream->ref(m_outputBuf2.data(), org_data_size);
#endif
                } else if (pkt->size > (decltype(pkt->size))sps_nal->size) {
                    pBitstream->trim();
                }
                memmove(pBitstream->data() + next_nal_new_offset, pBitstream->data() + next_nal_orig_offset, stream_orig_length - next_nal_orig_offset);
                memcpy(pBitstream->data() + sps_nal_offset, pkt->data, pkt->size);
                pBitstream->setSize(new_data_size);
                av_packet_unref(pkt);
            }
        }
#endif //#if ENABLE_AVSW_READER
        const bool isIDR = (pBitstream->frametype() & (RGY_FRAMETYPE_IDR | RGY_FRAMETYPE_xIDR)) != 0;
        writeRawDebug(pBitstream);
        if (m_VideoOutputInfo.codec == RGY_CODEC_AV1) {
            if (m_debugDirectAV1Out) {
                nBytesWritten = _fwrite_nolock(pBitstream->data(), 1, pBitstream->size(), m_fDest.get());
                WRITE_CHECK(nBytesWritten, pBitstream->size());
            } else {
                RGYTimestampMapVal bs_framedata;
                bool hdr10plus_metadata_written = false;

                const auto av1_units = parse_unit_av1(pBitstream->data(), pBitstream->size());
                for (size_t i = 0; i < av1_units.size(); i++) {
                    nBytesWritten += _fwrite_nolock(av1_units[i]->unit_data.data(), 1, av1_units[i]->unit_data.size(), m_fDest.get());

                    auto writeHdr10PlusMetadata = [&]() {
                        if (hdr10plus_metadata_written) {
                            return RGY_ERR_NONE;
                        }
                        const auto frameDataHdr10plusMetadata = std::find_if(bs_framedata.dataList.begin(), bs_framedata.dataList.end(),
                            [](const std::shared_ptr<RGYFrameData>& data) {
                            return data->dataType() == RGY_FRAME_DATA_HDR10PLUS;
                        });
                        if (frameDataHdr10plusMetadata != bs_framedata.dataList.end()) {
                            const auto frameDataPtr = dynamic_cast<RGYFrameDataHDR10plus *>((*frameDataHdr10plusMetadata).get());
                            if (!frameDataPtr) {
                                AddMessage(RGY_LOG_ERROR, _T("Invalid cast for hdr10plus metadata.\n"));
                                return RGY_ERR_UNSUPPORTED;
                            }
                            const auto hdr10plusMetadata = frameDataPtr->gen_obu();
                            if (hdr10plusMetadata.size() > 0) {
                                nBytesWritten += _fwrite_nolock(hdr10plusMetadata.data(), 1, hdr10plusMetadata.size(), m_fDest.get());
                            }
                        }
                        hdr10plus_metadata_written = true;
                        return RGY_ERR_NONE;
                    };

                    if (av1_units[i]->type == OBU_TEMPORAL_DELIMITER) {
                        //次のフレームの時刻情報を取得
                        bs_framedata = m_timestamp->getByEncodeFrameID(m_prevEncodeFrameId + 1);
                        if (bs_framedata.inputFrameId < 0) {
                            bs_framedata.inputFrameId = m_prevInputFrameId;
                            AddMessage(RGY_LOG_WARN, _T("Failed to get timestamp for id %lld, using %lld.\n"), pBitstream->pts(), bs_framedata.inputFrameId);
                        } else {
                            m_prevEncodeFrameId++;
                            m_prevInputFrameId = bs_framedata.inputFrameId;
                        }
                        hdr10plus_metadata_written = false;

                        if (i + 1 >= av1_units.size() || av1_units[i + 1]->type != OBU_SEQUENCE_HEADER) {
                            if (auto err = writeHdr10PlusMetadata(); err != RGY_ERR_NONE) {
                                return err;
                            }
                        }
                    } else if (av1_units[i]->type == OBU_SEQUENCE_HEADER) {
                        if (m_hdrBitstream.size() > 0 && (isIDR || av1_units[i]->type == OBU_SEQUENCE_HEADER)) {
                            nBytesWritten += _fwrite_nolock(m_hdrBitstream.data(), 1, m_hdrBitstream.size(), m_fDest.get());
                        }
                        if (auto err = writeHdr10PlusMetadata(); err != RGY_ERR_NONE) {
                            return err;
                        }
                    }
                }
                if (m_doviRpu) {
                    AddMessage(RGY_LOG_ERROR, _T("Adding dovi rpu not supported in %s encoding.\n"), CodecToStr(m_VideoOutputInfo.codec).c_str());
                    return RGY_ERR_UNSUPPORTED;
                }
            }
        } else {
            RGYTimestampMapVal bs_framedata;
            if (m_timestamp) {
                bs_framedata = m_timestamp->get(pBitstream->pts());
                if (bs_framedata.inputFrameId < 0) {
                    bs_framedata.inputFrameId = m_prevInputFrameId;
                    AddMessage(RGY_LOG_WARN, _T("Failed to get frame ID for pts %lld, using %lld.\n"), pBitstream->pts(), bs_framedata.inputFrameId);
                }
                m_prevInputFrameId = bs_framedata.inputFrameId;
            }
            if (bs_framedata.inputFrameId < 0) {
                AddMessage(RGY_LOG_ERROR, _T("Failed to get frame ID for pts %lld (%lld).\n"), pBitstream->pts(), bs_framedata.inputFrameId);
                return RGY_ERR_UNDEFINED_BEHAVIOR;
            }

            const auto frameDataHdr10plusMetadata = std::find_if(bs_framedata.dataList.begin(), bs_framedata.dataList.end(), [](std::shared_ptr<RGYFrameData>& data) {
                return data->dataType() == RGY_FRAME_DATA_HDR10PLUS;
            });
            std::vector<uint8_t> hdr10plusMetadata;
            if (frameDataHdr10plusMetadata != bs_framedata.dataList.end()) {
                auto frameDataPtr = dynamic_cast<RGYFrameDataHDR10plus *>((*frameDataHdr10plusMetadata).get());
                if (!frameDataPtr) {
                    AddMessage(RGY_LOG_ERROR, _T("Invalid cast for hdr10plus metadata.\n"));
                    return RGY_ERR_UNSUPPORTED;
                }
                if (m_VideoOutputInfo.codec == RGY_CODEC_HEVC) {
                    hdr10plusMetadata = frameDataPtr->gen_nal();
                } else {
                    AddMessage(RGY_LOG_ERROR, _T("Setting hdr10plus metadata not supported in %s encoding.\n"), CodecToStr(m_VideoOutputInfo.codec).c_str());
                    return RGY_ERR_UNSUPPORTED;
                }
            }

            const bool insertSEI = m_hdrBitstream.size() > 0 && isIDR;
            if (insertSEI || hdr10plusMetadata.size() > 0) {
                if (m_VideoOutputInfo.codec == RGY_CODEC_HEVC) {
                    const auto nal_list = parse_nal_hevc(pBitstream->data(), pBitstream->size());
                    const auto hevc_vps_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_HEVC_VPS; });
                    const auto hevc_sps_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_HEVC_SPS; });
                    const auto hevc_pps_nal = std::find_if(nal_list.begin(), nal_list.end(), [](nal_info info) { return info.type == NALU_HEVC_PPS; });
                    const bool header_check = (nal_list.end() != hevc_vps_nal) || (nal_list.end() != hevc_sps_nal) || (nal_list.end() != hevc_pps_nal);
                    bool seiWritten = false;
                    bool hdr10plus_metadata_written = false;
                    if (!header_check) {
                        if (m_hdrBitstream.size() > 0) {
                            nBytesWritten += _fwrite_nolock(m_hdrBitstream.data(), 1, m_hdrBitstream.size(), m_fDest.get());
                            seiWritten = true;
                        }
                        if (hdr10plusMetadata.size() > 0) {
                            nBytesWritten += _fwrite_nolock(hdr10plusMetadata.data(), 1, hdr10plusMetadata.size(), m_fDest.get());
                            hdr10plus_metadata_written = true;
                        }
                    }
                    for (size_t i = 0; i < nal_list.size(); i++) {
                        nBytesWritten += _fwrite_nolock(nal_list[i].ptr, 1, nal_list[i].size, m_fDest.get());
                        if (nal_list[i].type == NALU_HEVC_VPS || nal_list[i].type == NALU_HEVC_SPS || nal_list[i].type == NALU_HEVC_PPS) {
                            if (i + 1 < nal_list.size()
                                && (nal_list[i + 1].type != NALU_HEVC_VPS && nal_list[i + 1].type != NALU_HEVC_SPS && nal_list[i + 1].type != NALU_HEVC_PPS)) {
                                if (!seiWritten && insertSEI) {
                                    nBytesWritten += _fwrite_nolock(m_hdrBitstream.data(), 1, m_hdrBitstream.size(), m_fDest.get());
                                    seiWritten = true;
                                }
                                if (!hdr10plus_metadata_written && hdr10plusMetadata.size() > 0) {
                                    nBytesWritten += _fwrite_nolock(hdr10plusMetadata.data(), 1, hdr10plusMetadata.size(), m_fDest.get());
                                    hdr10plus_metadata_written = true;
                                }
                            }
                        }
                    }
                    if (insertSEI && !seiWritten) {
                        AddMessage(RGY_LOG_ERROR, _T("Unexpected HEVC header.\n"));
                        return RGY_ERR_UNDEFINED_BEHAVIOR;
                    }
                    if (hdr10plusMetadata.size() > 0 && !hdr10plus_metadata_written) {
                        AddMessage(RGY_LOG_ERROR, _T("hdr10plus metadata not written, unexpected behavior.\n"));
                        return RGY_ERR_UNDEFINED_BEHAVIOR;
                    }
                } else {
                    AddMessage(RGY_LOG_ERROR, _T("Setting masterdisplay/contentlight not supported in %s encoding.\n"), CodecToStr(m_VideoOutputInfo.codec).c_str());
                    return RGY_ERR_UNSUPPORTED;
                }
            } else {
                nBytesWritten = _fwrite_nolock(pBitstream->data(), 1, pBitstream->size(), m_fDest.get());
                WRITE_CHECK(nBytesWritten, pBitstream->size());
            }
            if (m_doviRpu) {
                if (m_VideoOutputInfo.codec == RGY_CODEC_HEVC) {
                    std::vector<uint8_t> dovi_nal;
                    if (m_doviRpu->get_next_rpu_nal(dovi_nal, bs_framedata.inputFrameId) != 0) {
                        AddMessage(RGY_LOG_ERROR, _T("Failed to get dovi rpu for %lld.\n"), bs_framedata.inputFrameId);
                    }
                    if (dovi_nal.size() > 0) {
                        nBytesWritten += _fwrite_nolock(dovi_nal.data(), 1, dovi_nal.size(), m_fDest.get());
                    }
                } else {
                    AddMessage(RGY_LOG_ERROR, _T("Adding dovi rpu not supported in %s encoding.\n"), CodecToStr(m_VideoOutputInfo.codec).c_str());
                    return RGY_ERR_UNSUPPORTED;
                }
            }
        }
    }

    m_encSatusInfo->SetOutputData(pBitstream->frametype(), nBytesWritten, 0);
    pBitstream->setSize(0);

    return RGY_ERR_NONE;
}

RGY_ERR RGYOutputRaw::WriteNextFrame(RGYFrame *pSurface) {
    UNREFERENCED_PARAMETER(pSurface);
    return RGY_ERR_UNSUPPORTED;
}

#if ENCODER_QSV

RGYOutFrame::RGYOutFrame() : m_bY4m(true) {
    m_strWriterName = _T("yuv writer");
    m_OutType = OUT_TYPE_SURFACE;
};

RGYOutFrame::~RGYOutFrame() {
};

RGY_ERR RGYOutFrame::Init(const TCHAR *strFileName, const VideoInfo *pVideoOutputInfo, const void *prm) {
    UNREFERENCED_PARAMETER(pVideoOutputInfo);
    if (_tcscmp(strFileName, _T("-")) == 0) {
        m_fDest.reset(stdout);
        m_outputIsStdout = true;
        AddMessage(RGY_LOG_DEBUG, _T("using stdout\n"));
    } else {
        FILE *fp = NULL;
        int error = _tfopen_s(&fp, strFileName, _T("wb"));
        if (0 != error || fp == NULL) {
            AddMessage(RGY_LOG_DEBUG, _T("failed to open file \"%s\": %s\n"), strFileName, _tcserror(error));
            return RGY_ERR_NULL_PTR;
        }
        m_fDest.reset(fp);
    }

    YUVWriterParam *writerParam = (YUVWriterParam *)prm;

    m_bY4m = writerParam->bY4m;
    m_sourceHWMem = true;
    m_inited = true;

    return RGY_ERR_NONE;
}

RGY_ERR RGYOutFrame::WriteNextFrame(RGYBitstream *pBitstream) {
    UNREFERENCED_PARAMETER(pBitstream);
    return RGY_ERR_UNSUPPORTED;
}

RGY_ERR RGYOutFrame::WriteNextFrame(RGYFrame *pSurface) {
    if (!m_fDest) {
        return RGY_ERR_NULL_PTR;
    }

    if (m_sourceHWMem) {
        if (m_readBuffer.get() == nullptr) {
            m_readBuffer.reset((uint8_t *)_aligned_malloc(pSurface->pitch() + 128, 16));
        }
    }

    if (m_bY4m) {
        if (!m_y4mHeaderWritten) {
            WriteY4MHeader(m_fDest.get(), &m_VideoOutputInfo);
            m_y4mHeaderWritten = true;
        }
        WRITE_CHECK(fwrite("FRAME\n", 1, strlen("FRAME\n"), m_fDest.get()), strlen("FRAME\n"));
    }

    auto loadLineToBuffer = [](uint8_t *ptrBuf, uint8_t *ptrSrc, int pitch) {
#if (defined(_M_IX86) || defined(_M_X64) || defined(__x86_64)) && ENCODER_QSV
        for (int i = 0; i < pitch; i += 128, ptrSrc += 128, ptrBuf += 128) {
            __m128i x0 = _mm_stream_load_si128((__m128i *)(ptrSrc +   0));
            __m128i x1 = _mm_stream_load_si128((__m128i *)(ptrSrc +  16));
            __m128i x2 = _mm_stream_load_si128((__m128i *)(ptrSrc +  32));
            __m128i x3 = _mm_stream_load_si128((__m128i *)(ptrSrc +  48));
            __m128i x4 = _mm_stream_load_si128((__m128i *)(ptrSrc +  64));
            __m128i x5 = _mm_stream_load_si128((__m128i *)(ptrSrc +  80));
            __m128i x6 = _mm_stream_load_si128((__m128i *)(ptrSrc +  96));
            __m128i x7 = _mm_stream_load_si128((__m128i *)(ptrSrc + 112));
            _mm_store_si128((__m128i *)(ptrBuf +   0), x0);
            _mm_store_si128((__m128i *)(ptrBuf +  16), x1);
            _mm_store_si128((__m128i *)(ptrBuf +  32), x2);
            _mm_store_si128((__m128i *)(ptrBuf +  48), x3);
            _mm_store_si128((__m128i *)(ptrBuf +  64), x4);
            _mm_store_si128((__m128i *)(ptrBuf +  80), x5);
            _mm_store_si128((__m128i *)(ptrBuf +  96), x6);
            _mm_store_si128((__m128i *)(ptrBuf + 112), x7);
        }
#else
        memcpy(ptrBuf, ptrSrc, pitch);
#endif
    };

    const uint32_t lumaWidthBytes = pSurface->width() << ((pSurface->csp() == RGY_CSP_P010) ? 1 : 0);
    if (   pSurface->csp() == RGY_CSP_YV12
        || pSurface->csp() == RGY_CSP_NV12
        || pSurface->csp() == RGY_CSP_P010) {
        const uint32_t cropOffset = pSurface->crop().e.up * pSurface->pitch() + pSurface->crop().e.left;
        if (m_sourceHWMem) {
            for (uint32_t j = 0; j < pSurface->height(); j++) {
                uint8_t *ptrBuf = m_readBuffer.get();
                uint8_t *ptrSrc = pSurface->ptrY() + (pSurface->crop().e.up + j) * pSurface->pitch();
                loadLineToBuffer(ptrBuf, ptrSrc, pSurface->pitch());
                WRITE_CHECK(fwrite(ptrBuf + pSurface->crop().e.left, 1, lumaWidthBytes, m_fDest.get()), lumaWidthBytes);
            }
        } else {
            for (uint32_t j = 0; j < pSurface->height(); j++) {
                WRITE_CHECK(fwrite(pSurface->ptrY() + cropOffset + j * pSurface->pitch(), 1, lumaWidthBytes, m_fDest.get()), lumaWidthBytes);
            }
        }
    }

    uint32_t frameSize = 0;
    if (pSurface->csp() == RGY_CSP_YV12) {
        frameSize = lumaWidthBytes * pSurface->height() * 3 / 2;

        uint32_t uvPitch = pSurface->pitch() >> 1;
        uint32_t uvWidth = pSurface->width() >> 1;
        uint32_t uvHeight = pSurface->height() >> 1;
        uint8_t *ptrBuf = m_readBuffer.get();
        for (uint32_t i = 0; i < uvHeight; i++) {
            loadLineToBuffer(ptrBuf, pSurface->ptrU() + (pSurface->crop().e.up + i) * uvPitch, uvPitch);
            WRITE_CHECK(fwrite(ptrBuf + (pSurface->crop().e.left >> 1), 1, uvWidth, m_fDest.get()), uvWidth);
        }
        for (uint32_t i = 0; i < uvHeight; i++) {
            loadLineToBuffer(ptrBuf, pSurface->ptrV() + (pSurface->crop().e.up + i) * uvPitch, uvPitch);
            WRITE_CHECK(fwrite(ptrBuf + (pSurface->crop().e.left >> 1), 1, uvWidth, m_fDest.get()), uvWidth);
        }
    } else if (pSurface->csp() == RGY_CSP_NV12) {
        frameSize = lumaWidthBytes * pSurface->height() * 3 / 2;
        uint32_t uvWidth = pSurface->width() >> 1;
        //uint32_t nv12Width = pSurface->width();
        uint32_t uvHeight = pSurface->height() >> 1;
        uint32_t uvFrameOffset = ALIGN16(uvWidth * uvHeight + 16);
        if (m_UVBuffer.get() == nullptr) {
            m_UVBuffer.reset((uint8_t *)_aligned_malloc(uvFrameOffset << 1, 32));
        }

        alignas(16) static const uint16_t MASK_LOW8[] = {
            0x00ff, 0x00ff, 0x00ff, 0x00ff, 0x00ff, 0x00ff, 0x00ff, 0x00ff
        };
        const __m128i xMaskLow8 = _mm_load_si128((__m128i *)MASK_LOW8);

        for (uint32_t j = 0; j < uvHeight; j++) {
            uint8_t *ptrBuf = m_readBuffer.get();
            uint8_t *ptrSrc = pSurface->ptrUV() + (pSurface->crop().e.up + j) * pSurface->pitch();
            if (m_sourceHWMem) {
                loadLineToBuffer(ptrBuf, ptrSrc, pSurface->pitch());
            } else {
                ptrBuf = ptrSrc;
            }

            uint8_t *ptrUV = ptrBuf + pSurface->crop().e.left;
            uint8_t *ptrU = m_UVBuffer.get() + j * uvWidth;
            uint8_t *ptrV = ptrU + uvFrameOffset;
#if defined(_M_IX86) || defined(_M_X64) || defined(__x86_64)
            for (uint32_t i = 0; i < uvWidth; i += 16, ptrUV += 32, ptrU += 16, ptrV += 16) {
                __m128i x0 = _mm_loadu_si128((__m128i *)(ptrUV +  0));
                __m128i x1 = _mm_loadu_si128((__m128i *)(ptrUV + 16));
                _mm_storeu_si128((__m128i *)ptrU, _mm_packus_epi16(_mm_and_si128(x0, xMaskLow8), _mm_and_si128(x1, xMaskLow8)));
                _mm_storeu_si128((__m128i *)ptrV, _mm_packus_epi16(_mm_srli_epi16(x0, 8), _mm_srli_epi16(x1, 8)));
            }
#else
            for (uint32_t i = 0; i < uvWidth; i++) {
                ptrU[i] = ptrUV[2*i+0];
                ptrV[i] = ptrUV[2*i+1];
            }
#endif
        }
        WRITE_CHECK(fwrite(m_UVBuffer.get(), 1, uvWidth * uvHeight, m_fDest.get()), uvWidth * uvHeight);
        WRITE_CHECK(fwrite(m_UVBuffer.get() + uvFrameOffset, 1, uvWidth * uvHeight, m_fDest.get()), uvWidth * uvHeight);
    } else if (pSurface->csp() == RGY_CSP_P010) {
        frameSize = lumaWidthBytes * pSurface->height() * 3 / 2;
        uint8_t *ptrBuf = m_readBuffer.get();
        for (uint32_t i = 0; i < (uint32_t)(pSurface->height() >> 1); i++) {
            loadLineToBuffer(ptrBuf, pSurface->ptrUV() + pSurface->crop().e.up * (pSurface->pitch() >> 1) + i * pSurface->pitch(), pSurface->pitch());
            WRITE_CHECK(fwrite(ptrBuf + pSurface->crop().e.left, 1, (uint32_t)pSurface->width() << 1, m_fDest.get()), (uint32_t)pSurface->width() << 1);
        }
    } else if (pSurface->csp() == RGY_CSP_RGB32R
        /*|| pSurface->csp() == 100 //DXGI_FORMAT_AYUV
        || pSurface->csp() == RGY_CSP_A2RGB10*/) {
        frameSize = lumaWidthBytes * pSurface->height() * 4;
        uint32_t w = pSurface->width();
        uint32_t h = pSurface->height();

        uint8_t *ptr = pSurface->ptrRGB() + pSurface->crop().e.left + pSurface->crop().e.up * pSurface->pitch();

        for (uint32_t i = 0; i < h; i++) {
            WRITE_CHECK(fwrite(ptr + i * pSurface->pitch(), 1, 4*w, m_fDest.get()), 4*w);
        }
    } else {
        return RGY_ERR_INVALID_COLOR_FORMAT;
    }

    m_encSatusInfo->SetOutputData(frametype_enc_to_rgy(MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_I), frameSize, 0);
    return RGY_ERR_NONE;
}

#endif //#if ENCODER_QSV

#include "rgy_input_sm.h"
#include "rgy_input_avcodec.h"
#include "rgy_output_avcodec.h"

std::unique_ptr<RGYHDRMetadata> createHEVCHDRSei(const std::string& maxCll, const std::string &masterDisplay, CspTransfer atcSei, const RGYInput *reader) {
    auto hdrMetadata = std::make_unique<RGYHDRMetadata>();
    const AVMasteringDisplayMetadata *masteringDisplaySrc = nullptr;
    const AVContentLightMetadata *contentLightSrc = nullptr;
    { auto avcodecReader = dynamic_cast<const RGYInputAvcodec *>(reader);
        if (avcodecReader != nullptr) {
            masteringDisplaySrc = avcodecReader->getMasteringDisplay();
            contentLightSrc = avcodecReader->getContentLight();
        }
    }
    int ret = 0;
    if (maxCll == maxCLLSource) {
        if (contentLightSrc != nullptr) {
            hdrMetadata->set_maxcll(contentLightSrc->MaxCLL, contentLightSrc->MaxFALL);
        }
    } else {
        ret = hdrMetadata->parse_maxcll(maxCll);
    }
    if (masterDisplay == masterDisplaySource) {
        if (masteringDisplaySrc != nullptr) {
            rgy_rational<int> masterdisplay[10];
            masterdisplay[0] = to_rgy(masteringDisplaySrc->display_primaries[1][0]); //G
            masterdisplay[1] = to_rgy(masteringDisplaySrc->display_primaries[1][1]); //G
            masterdisplay[2] = to_rgy(masteringDisplaySrc->display_primaries[2][0]); //B
            masterdisplay[3] = to_rgy(masteringDisplaySrc->display_primaries[2][1]); //B
            masterdisplay[4] = to_rgy(masteringDisplaySrc->display_primaries[0][0]); //R
            masterdisplay[5] = to_rgy(masteringDisplaySrc->display_primaries[0][1]); //R
            masterdisplay[6] = to_rgy(masteringDisplaySrc->white_point[0]);
            masterdisplay[7] = to_rgy(masteringDisplaySrc->white_point[1]);
            masterdisplay[8] = to_rgy(masteringDisplaySrc->max_luminance);
            masterdisplay[9] = to_rgy(masteringDisplaySrc->min_luminance);
            hdrMetadata->set_masterdisplay(masterdisplay);
        }
    } else {
        ret = hdrMetadata->parse_masterdisplay(masterDisplay);
    }
    if (atcSei != RGY_TRANSFER_UNKNOWN) {
        hdrMetadata->set_atcsei(atcSei);
    }
    if (ret) {
        hdrMetadata.reset();
    }
    return hdrMetadata;
}

static bool audioSelected(const AudioSelect *sel, const AVDemuxStream *stream) {
    if (sel->trackID == trackID(stream->trackId)) {
        return true;
    }
    if (sel->trackID == TRACK_SELECT_BY_LANG && rgy_lang_equal(sel->lang, stream->lang)) {
        return true;
    }
    if (sel->trackID == TRACK_SELECT_BY_CODEC && stream->stream != nullptr && avcodec_equal(sel->selectCodec, stream->stream->codecpar->codec_id)) {
        return true;
    }
    return false;
};
static bool subSelected(const SubtitleSelect *sel, const AVDemuxStream *stream) {
    if (sel->trackID == trackID(stream->trackId)) {
        return true;
    }
    if (sel->trackID == TRACK_SELECT_BY_LANG && rgy_lang_equal(sel->lang, stream->lang)) {
        return true;
    }
    if (sel->trackID == TRACK_SELECT_BY_CODEC && stream->stream != nullptr && avcodec_equal(sel->selectCodec, stream->stream->codecpar->codec_id)) {
        return true;
    }
    return false;
};
static bool dataSelected(const DataSelect *sel, const AVDemuxStream *stream) {
    if (sel->trackID == trackID(stream->trackId)) {
        return true;
    }
    if (sel->trackID == TRACK_SELECT_BY_LANG && rgy_lang_equal(sel->lang, stream->lang)) {
        return true;
    }
    if (sel->trackID == TRACK_SELECT_BY_CODEC && stream->stream != nullptr && avcodec_equal(sel->selectCodec, stream->stream->codecpar->codec_id)) {
        return true;
    }
    return false;
};

RGY_ERR initWriters(
    shared_ptr<RGYOutput> &pFileWriter,
    vector<shared_ptr<RGYOutput>>& pFileWriterListAudio,
    shared_ptr<RGYInput> &pFileReader,
    vector<shared_ptr<RGYInput>> &otherReaders,
    RGYParamCommon *common,
    const VideoInfo *input,
    const RGYParamControl *ctrl,
    const VideoInfo outputVideoInfo,
    const sTrimParam& trimParam,
    const rgy_rational<int> outputTimebase,
#if ENABLE_AVSW_READER
    const vector<unique_ptr<AVChapter>>& chapters,
#endif //#if ENABLE_AVSW_READER
    const RGYHDRMetadata *hdrMetadata,
    DOVIRpu *doviRpu,
    RGYTimestamp *vidTimestamp,
    const bool videoDtsUnavailable,
    const bool benchmark,
    RGYPoolAVPacket *poolPkt,
    RGYPoolAVFrame *poolFrame,
    shared_ptr<EncodeStatus> pStatus,
    shared_ptr<CPerfMonitor> pPerfMonitor,
    shared_ptr<RGYLog> log
) {
    bool stdoutUsed = false;
#if ENABLE_AVSW_READER
    vector<int> streamTrackUsed; //使用した音声/字幕のトラックIDを保存する
    bool useH264ESOutput =
        ((common->muxOutputFormat.length() > 0 && 0 == _tcscmp(common->muxOutputFormat.c_str(), _T("raw")))) //--formatにrawが指定されている
        || std::filesystem::path(common->outputFilename).extension().empty() //拡張子がない
        || check_ext(common->outputFilename.c_str(), { ".m2v", ".264", ".h264", ".avc", ".avc1", ".x264", ".265", ".h265", ".hevc", ".vp9", ".av1", ".raw" }); //特定の拡張子
    if (!useH264ESOutput && outputVideoInfo.codec != RGY_CODEC_UNKNOWN) {
        common->AVMuxTarget |= RGY_MUX_VIDEO;
    }


    double inputFileDuration = 0.0;
    { auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(pFileReader);
    if (pAVCodecReader != nullptr) {
        //caption2ass用の解像度情報の提供
        //これをしないと入力ファイルのデータをずっとバッファし続けるので注意
        pAVCodecReader->setOutputVideoInfo(outputVideoInfo.dstWidth, outputVideoInfo.dstHeight,
            outputVideoInfo.sar[0], outputVideoInfo.sar[1],
            (common->AVMuxTarget & RGY_MUX_VIDEO) != 0);
        inputFileDuration = pAVCodecReader->GetInputVideoDuration();
    }
    }
    bool isAfs = false;
#if ENABLE_SM_READER
    { auto pReaderSM = std::dynamic_pointer_cast<RGYInputSM>(pFileReader);
    if (pReaderSM) {
        isAfs = pReaderSM->isAfs();
    }
    }
#endif //#if ENABLE_SM_READER
    //if (inputParams->CodecId == MFX_CODEC_RAW) {
    //    inputParams->AVMuxTarget &= ~RGY_MUX_VIDEO;
    //}
    pStatus->Init(outputVideoInfo.fpsN, outputVideoInfo.fpsD, input->frames, inputFileDuration, trimParam, log, pPerfMonitor);
    if (ctrl->perfMonitorSelect || ctrl->perfMonitorSelectMatplot) {
        pPerfMonitor->SetEncStatus(pStatus);
    }

    bool audioCopyAll = false;
    if (common->AVMuxTarget & RGY_MUX_VIDEO) {
        log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Using avformat writer.\n"));
        pFileWriter = std::make_shared<RGYOutputAvcodec>();
        AvcodecWriterPrm writerPrm;
        writerPrm.outputFormat            = common->muxOutputFormat;
        writerPrm.trimList                = trimParam.list;
        writerPrm.bVideoDtsUnavailable    = videoDtsUnavailable;
        writerPrm.threadOutput           = ctrl->threadOutput;
        writerPrm.threadAudio            = ctrl->threadAudio;
        writerPrm.threadParamOutput      = ctrl->threadParams.get(RGYThreadType::OUTUT);
        writerPrm.threadParamAudio       = ctrl->threadParams.get(RGYThreadType::AUDIO);
        writerPrm.bufSizeMB              = ctrl->outputBufSizeMB;
        writerPrm.audioResampler         = common->audioResampler;
        writerPrm.audioIgnoreDecodeError = common->audioIgnoreDecodeError;
        writerPrm.queueInfo = (pPerfMonitor) ? pPerfMonitor->GetQueueInfoPtr() : nullptr;
        writerPrm.muxVidTsLogFile         = (ctrl->logMuxVidTsFile) ? ctrl->logMuxVidTsFile : _T("");
        writerPrm.bitstreamTimebase       = av_make_q(outputTimebase);
        writerPrm.chapterNoTrim           = common->chapterNoTrim;
        writerPrm.attachments             = common->attachmentSource;
        writerPrm.hdrMetadata             = hdrMetadata;
        writerPrm.doviRpu                 = doviRpu;
        writerPrm.vidTimestamp            = vidTimestamp;
        writerPrm.videoCodecTag           = common->videoCodecTag;
        writerPrm.videoMetadata           = common->videoMetadata;
        writerPrm.formatMetadata          = common->formatMetadata;
        writerPrm.afs                     = isAfs;
        writerPrm.disableMp4Opt           = common->disableMp4Opt;
        writerPrm.lowlatency              = ctrl->lowLatency;
        writerPrm.debugDirectAV1Out       = common->debugDirectAV1Out;
        writerPrm.muxOpt                  = common->muxOpt;
        writerPrm.poolPkt                 = poolPkt;
        writerPrm.poolFrame               = poolFrame;
        auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(pFileReader);
        if (pAVCodecReader != nullptr) {
            writerPrm.inputFormatMetadata = pAVCodecReader->GetInputFormatMetadata();
            writerPrm.videoInputFirstKeyPts = pAVCodecReader->GetVideoFirstKeyPts();
            writerPrm.videoInputStream = pAVCodecReader->GetInputVideoStream();
        }
        if (chapters.size() > 0 && (common->copyChapter || common->chapterFile.length() > 0)) {
            writerPrm.chapterList.clear();
            for (uint32_t i = 0; i < chapters.size(); i++) {
                writerPrm.chapterList.push_back(chapters[i].get());
            }
        }
        if (common->AVMuxTarget & (RGY_MUX_AUDIO | RGY_MUX_SUBTITLE)) {
            log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Audio/Subtitle muxing enabled.\n"));
            for (int i = 0; !audioCopyAll && i < common->nAudioSelectCount; i++) {
                //トラック"0"が指定されていれば、すべてのトラックをコピーするということ
                audioCopyAll = (common->ppAudioSelectList[i]->trackID == 0);
            }
            log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: CopyAll=%s\n"), (audioCopyAll) ? _T("true") : _T("false"));
            pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(pFileReader);
            vector<AVDemuxStream> streamList = pFileReader->GetInputStreamInfo();

            for (auto& stream : streamList) {
                const auto streamMediaType = trackMediaType(stream.trackId);
                //audio-fileで別ファイルとして抽出するものは除く
                bool usedInAudioFile = false;
                for (int i = 0; i < (int)common->nAudioSelectCount; i++) {
                    if (audioSelected(common->ppAudioSelectList[i], &stream)
                        && common->ppAudioSelectList[i]->extractFilename.length() > 0) {
                        usedInAudioFile = true;
                    }
                }
                if (usedInAudioFile) {
                    continue;
                }
                const AudioSelect *pAudioSelect = nullptr;
                if (streamMediaType == AVMEDIA_TYPE_AUDIO) {
                    for (int i = 0; i < (int)common->nAudioSelectCount; i++) {
                        if (audioSelected(common->ppAudioSelectList[i], &stream)
                            && common->ppAudioSelectList[i]->extractFilename.length() == 0) {
                            pAudioSelect = common->ppAudioSelectList[i];
                            break;
                        }
                    }
                    if (pAudioSelect == nullptr) {
                        //一致するTrackIDがなければ、trackID = 0 (全指定)を探す
                        for (int i = 0; i < common->nAudioSelectCount; i++) {
                            if (common->ppAudioSelectList[i]->trackID == 0
                                && common->ppAudioSelectList[i]->extractFilename.length() == 0) {
                                pAudioSelect = common->ppAudioSelectList[i];
                                break;
                            }
                        }
                    }
                }
                const SubtitleSelect *pSubtitleSelect = nullptr;
                if (streamMediaType == AVMEDIA_TYPE_SUBTITLE) {
                    for (int i = 0; i < common->nSubtitleSelectCount; i++) {
                        if (subSelected(common->ppSubtitleSelectList[i], &stream)) {
                            pSubtitleSelect = common->ppSubtitleSelectList[i];
                            break;
                        }
                    }
                    if (pSubtitleSelect == nullptr) {
                        //一致するTrackIDがなければ、trackID = 0 (全指定)を探す
                        for (int i = 0; i < common->nSubtitleSelectCount; i++) {
                            if (common->ppSubtitleSelectList[i]->trackID == 0) {
                                pSubtitleSelect = common->ppSubtitleSelectList[i];
                                break;
                            }
                        }
                    }
                }
                const DataSelect *pDataSelect = nullptr;
                if (streamMediaType == AVMEDIA_TYPE_DATA) {
                    for (int i = 0; i < common->nDataSelectCount; i++) {
                        if (dataSelected(common->ppDataSelectList[i], &stream)) {
                            pDataSelect = common->ppDataSelectList[i];
                        }
                    }
                    if (pSubtitleSelect == nullptr) {
                        //一致するTrackIDがなければ、trackID = 0 (全指定)を探す
                        for (int i = 0; i < common->nDataSelectCount; i++) {
                            if (common->ppDataSelectList[i]->trackID == 0) {
                                pDataSelect = common->ppDataSelectList[i];
                                break;
                            }
                        }
                    }
                }
                if (pAudioSelect != nullptr || audioCopyAll || streamMediaType != AVMEDIA_TYPE_AUDIO) {
                    streamTrackUsed.push_back(stream.trackId);
                    if (pSubtitleSelect == nullptr && streamMediaType == AVMEDIA_TYPE_SUBTITLE) {
                        if (common->caption2ass == FORMAT_INVALID) { //caption2assの字幕の場合はそのまま処理する
                            continue;
                        }
                        //caption2assの字幕の場合、AVOutputStreamPrmのパラメータはデフォルト(copy)でよい
                    }
                    AVOutputStreamPrm prm;
                    prm.src = stream;
                    //pAudioSelect == nullptrは "copyAllStreams" か 字幕ストリーム によるもの
                    if (pAudioSelect != nullptr) {
                        prm.decodeCodecPrm = pAudioSelect->decCodecPrm;
                        prm.bitrate = pAudioSelect->encBitrate;
                        prm.samplingRate = pAudioSelect->encSamplingRate;
                        prm.encodeCodec = pAudioSelect->encCodec;
                        prm.encodeCodecPrm = pAudioSelect->encCodecPrm;
                        prm.encodeCodecProfile = pAudioSelect->encCodecProfile;
                        prm.filter = pAudioSelect->filter;
                        prm.bsf = pAudioSelect->bsf;
                        prm.disposition = pAudioSelect->disposition;
                        prm.metadata = pAudioSelect->metadata;
                    }
                    if (pSubtitleSelect != nullptr) {
                        prm.decodeCodecPrm = pSubtitleSelect->decCodecPrm;
                        prm.encodeCodec = pSubtitleSelect->encCodec;
                        prm.encodeCodecPrm = pSubtitleSelect->encCodecPrm;
                        prm.asdata = pSubtitleSelect->asdata;
                        prm.bsf = pSubtitleSelect->bsf;
                        prm.disposition = pSubtitleSelect->disposition;
                        prm.metadata = pSubtitleSelect->metadata;
                    }
                    if (pDataSelect != nullptr) {
                        prm.disposition = pDataSelect->disposition;
                        prm.metadata = pDataSelect->metadata;
                    }
                    log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Added %s track#%d (stream idx %d) for mux, bitrate %d, codec: %s %s %s, bsf: %s, disposition: %s, metadata %s\n"),
                        char_to_tstring(av_get_media_type_string(streamMediaType)).c_str(),
                        stream.trackId, stream.index, prm.bitrate, prm.encodeCodec.c_str(),
                        prm.encodeCodecProfile.c_str(),
                        prm.encodeCodecPrm.c_str(),
                        prm.bsf.length() > 0 ? prm.bsf.c_str() : _T("<none>"),
                        prm.disposition.length() > 0 ? prm.disposition.c_str() : _T("<copy>"),
                        prm.metadata.size() > 0 ? print_metadata(prm.metadata).c_str() : _T("<copy>"));
                    writerPrm.inputStreamList.push_back(std::move(prm));
                }
            }
            vector<AVDemuxStream> otherSrcStreams;
            for (const auto &reader : otherReaders) {
                if (reader->GetAudioTrackCount() > 0 || reader->GetSubtitleTrackCount() > 0) {
                    auto pAVCodecAudioReader = std::dynamic_pointer_cast<RGYInputAvcodec>(reader);
                    if (pAVCodecAudioReader) {
                        vector_cat(otherSrcStreams, pAVCodecAudioReader->GetInputStreamInfo());
                    }
                    //もしavqsvリーダーでないなら、音声リーダーから情報を取得する必要がある
                    if (pAVCodecReader == nullptr) {
                        writerPrm.videoInputFirstKeyPts = pAVCodecAudioReader->GetVideoFirstKeyPts();
                        writerPrm.videoInputStream = pAVCodecAudioReader->GetInputVideoStream();
                    }
                }
            }
            for (auto &stream : otherSrcStreams) {
                const auto streamMediaType = trackMediaType(stream.trackId);
                if (stream.sourceFileIndex < 0) {
                    log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, _T("Internal Error, Invalid file index %d set for %s-source.\n"),
                        stream.sourceFileIndex, char_to_tstring(av_get_media_type_string(streamMediaType)).c_str());
                    return RGY_ERR_UNKNOWN;
                }
                //audio-fileで別ファイルとして抽出するものは除く
                if (streamMediaType == AVMEDIA_TYPE_AUDIO) {
                    bool usedInAudioFile = false;
                    const auto& audsrc = common->audioSource[stream.sourceFileIndex];
                    for (const auto& audsel : audsrc.select) {
                        if (audioSelected(&audsel.second, &stream)
                            && audsel.second.extractFilename.length() > 0) {
                            usedInAudioFile = true;
                        }
                    }
                    if (usedInAudioFile) {
                        continue;
                    }
                }
                const AudioSelect *pAudioSelect = nullptr;
                if (streamMediaType == AVMEDIA_TYPE_AUDIO) {
                    if (stream.sourceFileIndex >= (int)common->audioSource.size()) {
                        log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, _T("Internal Error, Invalid file index %d set for audio-source.\n"), stream.sourceFileIndex);
                        return RGY_ERR_UNKNOWN;
                    }
                    const auto& audsrc = common->audioSource[stream.sourceFileIndex];
                    for (const auto &audsel : audsrc.select) {
                        if (audioSelected(&audsel.second, &stream)) {
                            pAudioSelect = &audsel.second;
                            break;
                        }
                    }
                    if (pAudioSelect == nullptr) {
                        //一致するTrackIDがなければ、trackID = 0 (全指定)を探す
                        for (const auto& audsel : audsrc.select) {
                            if (audsel.first == 0) {
                                pAudioSelect = &audsel.second;
                                break;
                            }
                        }
                    }
                }
                const SubtitleSelect *pSubtitleSelect = nullptr;
                if (streamMediaType == AVMEDIA_TYPE_SUBTITLE) {
                    if (stream.sourceFileIndex >= (int)common->subSource.size()) {
                        log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, _T("Internal Error, Invalid file index %d set for audio-source.\n"), stream.sourceFileIndex);
                        return RGY_ERR_UNKNOWN;
                    }
                    const auto& subsrc = common->subSource[stream.sourceFileIndex];
                    for (const auto &subsel : subsrc.select) {
                        if (subSelected(&subsel.second, &stream)) {
                            pSubtitleSelect = &subsel.second;
                            break;
                        }
                    }
                    if (pSubtitleSelect == nullptr) {
                        //一致するTrackIDがなければ、trackID = 0 (全指定)を探す
                        for (const auto &subsel : subsrc.select) {
                            if (subsel.first == 0) {
                                pSubtitleSelect = &subsel.second;
                                break; //2重ループをbreak
                            }
                        }
                    }
                }
                if (pAudioSelect != nullptr || audioCopyAll || streamMediaType != AVMEDIA_TYPE_AUDIO) {
                    streamTrackUsed.push_back(stream.trackId);
                    AVOutputStreamPrm prm;
                    prm.src = stream;
                    //pAudioSelect == nullptrは "copyAllStreams" か 字幕ストリーム によるもの
                    if (pAudioSelect != nullptr) {
                        prm.decodeCodecPrm = pAudioSelect->decCodecPrm;
                        prm.bitrate = pAudioSelect->encBitrate;
                        prm.samplingRate = pAudioSelect->encSamplingRate;
                        prm.encodeCodec = pAudioSelect->encCodec;
                        prm.encodeCodecPrm = pAudioSelect->encCodecPrm;
                        prm.encodeCodecProfile = pAudioSelect->encCodecProfile;
                        prm.filter = pAudioSelect->filter;
                        prm.bsf = pAudioSelect->bsf;
                        prm.disposition = pAudioSelect->disposition;
                        prm.metadata = pAudioSelect->metadata;
                    }
                    if (pSubtitleSelect != nullptr) {
                        prm.decodeCodecPrm = pSubtitleSelect->decCodecPrm;
                        prm.encodeCodec = pSubtitleSelect->encCodec;
                        prm.encodeCodecPrm = pSubtitleSelect->encCodecPrm;
                        prm.asdata = pSubtitleSelect->asdata;
                        prm.bsf = pSubtitleSelect->bsf;
                        prm.disposition = pSubtitleSelect->disposition;
                        prm.metadata = pSubtitleSelect->metadata;
                    }
                    log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Added %s track#%d (stream idx %d) for mux, bitrate %d, codec: %s %s %s, bsf: %s, disposition: %s, metadata: %s\n"),
                        char_to_tstring(av_get_media_type_string(streamMediaType)).c_str(),
                        stream.trackId, stream.index, prm.bitrate, prm.encodeCodec.c_str(),
                        prm.encodeCodecProfile.c_str(),
                        prm.encodeCodecPrm.c_str(),
                        prm.bsf.length() > 0 ? prm.bsf.c_str() : _T("<none>"),
                        prm.disposition.length() > 0 ? prm.disposition.c_str() : _T("<copy>"),
                        prm.metadata.size() > 0 ? print_metadata(prm.metadata).c_str() : _T("<copy>"));
                    writerPrm.inputStreamList.push_back(std::move(prm));
                }
            }
            vector_cat(streamList, otherSrcStreams);
        }
        auto sts = pFileWriter->Init(common->outputFilename.c_str(), &outputVideoInfo, &writerPrm, log, pStatus);
        if (sts != RGY_ERR_NONE) {
            log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, pFileWriter->GetOutputMessage());
            return sts;
        } else if (common->AVMuxTarget & (RGY_MUX_AUDIO | RGY_MUX_SUBTITLE)) {
            pFileWriterListAudio.push_back(pFileWriter);
        }
        stdoutUsed = pFileWriter->outputStdout();
        log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Initialized avformat writer%s.\n"), (stdoutUsed) ? _T("using stdout") : _T(""));
    } else if (common->AVMuxTarget & (RGY_MUX_AUDIO | RGY_MUX_SUBTITLE)) {
        log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, _T("Audio mux cannot be used alone, should be use with video mux.\n"));
        return RGY_ERR_UNKNOWN;
    } else {
#endif //ENABLE_AVSW_READER
#if ENCODER_QSV
        if (outputVideoInfo.codec == RGY_CODEC_UNKNOWN) {
            pFileWriter = std::make_shared<RGYOutFrame>();
            YUVWriterParam param;
            param.bY4m = true;
            auto sts = pFileWriter->Init(common->outputFilename.c_str(), &outputVideoInfo, &param, log, pStatus);
            if (sts != RGY_ERR_NONE) {
                log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, pFileWriter->GetOutputMessage());
                return sts;
            }
            stdoutUsed = pFileWriter->outputStdout();
            log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Initialized yuv frame writer%s.\n"), (stdoutUsed) ? _T("using stdout") : _T(""));
        } else
#endif
        {
            pFileWriter = std::make_shared<RGYOutputRaw>();
            RGYOutputRawPrm rawPrm;
            rawPrm.bufSizeMB = ctrl->outputBufSizeMB;
            rawPrm.benchmark = benchmark;
            rawPrm.codecId = outputVideoInfo.codec;
            rawPrm.hdrMetadata = hdrMetadata;
            rawPrm.doviRpu = doviRpu;
            rawPrm.vidTimestamp = vidTimestamp;
            rawPrm.debugDirectAV1Out = common->debugDirectAV1Out;
            rawPrm.debugRawOut = common->debugRawOut;
            rawPrm.outReplayFile = common->outReplayFile;
            rawPrm.outReplayCodec = common->outReplayCodec;
            auto sts = pFileWriter->Init(common->outputFilename.c_str(), &outputVideoInfo, &rawPrm, log, pStatus);
            if (sts != RGY_ERR_NONE) {
                log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, pFileWriter->GetOutputMessage());
                return sts;
            }
            stdoutUsed = pFileWriter->outputStdout();
            log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Initialized bitstream writer%s.\n"), (stdoutUsed) ? _T("using stdout") : _T(""));
        }
#if ENABLE_AVSW_READER
    }

    //音声の抽出(--audio-file)
    const bool hasAudioExtract = std::find_if(common->ppAudioSelectList, common->ppAudioSelectList + common->nAudioSelectCount,
        [](const AudioSelect *pAudioSelect) { return pAudioSelect->extractFilename.length() > 0; }) != common->ppAudioSelectList + common->nAudioSelectCount;
    if (hasAudioExtract
        && common->nAudioSelectCount + common->nSubtitleSelectCount - (audioCopyAll ? 1 : 0) > (int)streamTrackUsed.size()) {
        log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Audio file output enabled.\n"));
        auto pAVCodecReader = std::dynamic_pointer_cast<RGYInputAvcodec>(pFileReader);
        if (pAVCodecReader == nullptr) {
            log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, _T("Audio output is only supported with transcoding (avhw/avsw reader).\n"));
            return RGY_ERR_INVALID_PARAM;
        } else {
            auto inutAudioInfoList = pAVCodecReader->GetInputStreamInfo();
            for (auto& audioTrack : inutAudioInfoList) {
                bool bTrackAlreadyUsed = false;
                for (auto usedTrack : streamTrackUsed) {
                    if (usedTrack == audioTrack.trackId) {
                        bTrackAlreadyUsed = true;
                        log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Audio track #%d is already set to be muxed, so cannot be extracted to file.\n"), trackID(audioTrack.trackId));
                        break;
                    }
                }
                if (bTrackAlreadyUsed) {
                    continue;
                }
                const AudioSelect *pAudioSelect = nullptr;
                for (int i = 0; i < (int)common->nAudioSelectCount; i++) {
                    if (audioSelected(common->ppAudioSelectList[i], &audioTrack)
                        && common->ppAudioSelectList[i]->extractFilename.length() > 0) {
                        pAudioSelect = common->ppAudioSelectList[i];
                    }
                }
                if (pAudioSelect == nullptr) {
                    log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, _T("Audio track #%d is not used anyware, this should not happen.\n"), trackID(audioTrack.trackId));
                    return RGY_ERR_UNKNOWN;
                }
                log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Output audio track #%d (stream index %d) to \"%s\", format: %s, codec %s, bitrate %d\n"),
                    trackID(audioTrack.trackId), audioTrack.index, pAudioSelect->extractFilename.c_str(), pAudioSelect->extractFormat.c_str(), pAudioSelect->encCodec.c_str(), pAudioSelect->encBitrate);

                AVOutputStreamPrm prm;
                prm.src = audioTrack;
                //pAudioSelect == nullptrは "copyAll" によるもの
                prm.bitrate = pAudioSelect->encBitrate;
                prm.filter = pAudioSelect->filter;
                prm.encodeCodec = pAudioSelect->encCodec;
                prm.samplingRate = pAudioSelect->encSamplingRate;

                AvcodecWriterPrm writerAudioPrm;
                writerAudioPrm.threadOutput   = ctrl->threadOutput;
                writerAudioPrm.threadAudio    = ctrl->threadAudio;
                writerAudioPrm.threadParamOutput = ctrl->threadParams.get(RGYThreadType::OUTUT);
                writerAudioPrm.threadParamAudio  = ctrl->threadParams.get(RGYThreadType::AUDIO);
                writerAudioPrm.bufSizeMB      = ctrl->outputBufSizeMB;
                writerAudioPrm.outputFormat   = pAudioSelect->extractFormat;
                writerAudioPrm.audioIgnoreDecodeError = common->audioIgnoreDecodeError;
                writerAudioPrm.lowlatency = ctrl->lowLatency;
                writerAudioPrm.audioResampler = common->audioResampler;
                writerAudioPrm.inputStreamList.push_back(prm);
                writerAudioPrm.trimList = trimParam.list;
                writerAudioPrm.videoInputFirstKeyPts = pAVCodecReader->GetVideoFirstKeyPts();
                writerAudioPrm.videoInputStream = pAVCodecReader->GetInputVideoStream();
                writerAudioPrm.bitstreamTimebase = av_make_q(outputTimebase);
                writerAudioPrm.poolPkt = poolPkt;
                writerAudioPrm.poolFrame = poolFrame;

                shared_ptr<RGYOutput> pWriter = std::make_shared<RGYOutputAvcodec>();
                auto sts = pWriter->Init(pAudioSelect->extractFilename.c_str(), nullptr, &writerAudioPrm, log, pStatus);
                if (sts != RGY_ERR_NONE) {
                    log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, pWriter->GetOutputMessage());
                    return sts;
                }
                log->write(RGY_LOG_DEBUG, RGY_LOGT_OUT, _T("Output: Intialized audio output for track #%d.\n"), trackID(audioTrack.trackId));
                bool audioStdout = pWriter->outputStdout();
                if (stdoutUsed && audioStdout) {
                    log->write(RGY_LOG_ERROR, RGY_LOGT_OUT, _T("Multiple stream outputs are set to stdout, please remove conflict.\n"));
                    return RGY_ERR_UNKNOWN;
                }
                stdoutUsed |= audioStdout;
                pFileWriterListAudio.push_back(std::move(pWriter));
            }
        }
    }
#endif //ENABLE_AVSW_READER
    return RGY_ERR_NONE;
}
