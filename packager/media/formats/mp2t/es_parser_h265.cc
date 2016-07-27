// Copyright 2016 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/formats/mp2t/es_parser_h265.h"

#include <stdint.h>

#include "packager/base/logging.h"
#include "packager/media/base/media_sample.h"
#include "packager/media/base/offset_byte_queue.h"
#include "packager/media/base/timestamp.h"
#include "packager/media/base/video_stream_info.h"
#include "packager/media/codecs/h265_byte_to_unit_stream_converter.h"
#include "packager/media/codecs/h265_parser.h"
#include "packager/media/codecs/hevc_decoder_configuration_record.h"
#include "packager/media/formats/mp2t/mp2t_common.h"

namespace shaka {
namespace media {
namespace mp2t {

EsParserH265::EsParserH265(uint32_t pid,
                           const NewStreamInfoCB& new_stream_info_cb,
                           const EmitSampleCB& emit_sample_cb)
    : EsParserH26x(Nalu::kH265,
                   scoped_ptr<H26xByteToUnitStreamConverter>(
                       new H265ByteToUnitStreamConverter()),
                   pid,
                   emit_sample_cb),
      new_stream_info_cb_(new_stream_info_cb),
      decoder_config_check_pending_(false),
      h265_parser_(new H265Parser()) {}

EsParserH265::~EsParserH265() {}

void EsParserH265::Reset() {
  DVLOG(1) << "EsParserH265::Reset";
  h265_parser_.reset(new H265Parser());
  last_video_decoder_config_ = scoped_refptr<StreamInfo>();
  decoder_config_check_pending_ = false;
  EsParserH26x::Reset();
}

bool EsParserH265::ProcessNalu(const Nalu& nalu,
                               bool* is_key_frame,
                               int* pps_id_for_access_unit) {
  switch (nalu.type()) {
    case Nalu::H265_AUD: {
      DVLOG(LOG_LEVEL_ES) << "Nalu: AUD";
      break;
    }
    case Nalu::H265_SPS: {
      DVLOG(LOG_LEVEL_ES) << "Nalu: SPS";
      int sps_id;
      if (h265_parser_->ParseSps(nalu, &sps_id) != H265Parser::kOk)
        return false;
      decoder_config_check_pending_ = true;
      break;
    }
    case Nalu::H265_PPS: {
      DVLOG(LOG_LEVEL_ES) << "Nalu: PPS";
      int pps_id;
      if (h265_parser_->ParsePps(nalu, &pps_id) != H265Parser::kOk) {
        // Allow PPS parsing to fail if waiting for SPS.
        if (last_video_decoder_config_)
          return false;
      } else {
        decoder_config_check_pending_ = true;
      }
      break;
    }
    default: {
      if (nalu.is_video_slice()) {
        *is_key_frame = nalu.type() == Nalu::H265_IDR_W_RADL ||
                        nalu.type() == Nalu::H265_IDR_N_LP;
        DVLOG(LOG_LEVEL_ES) << "Nalu: slice KeyFrame=" << is_key_frame;
        H265SliceHeader shdr;
        if (h265_parser_->ParseSliceHeader(nalu, &shdr) != H265Parser::kOk) {
          // Only accept an invalid SPS/PPS at the beginning when the stream
          // does not necessarily start with an SPS/PPS/IDR.
          if (last_video_decoder_config_)
            return false;
        } else {
          *pps_id_for_access_unit = shdr.pic_parameter_set_id;
        }
      } else {
        DVLOG(LOG_LEVEL_ES) << "Nalu: " << nalu.type();
      }
    }
  }

  return true;
}

bool EsParserH265::UpdateVideoDecoderConfig(int pps_id) {
  // Update the video decoder configuration if needed.
  if (!decoder_config_check_pending_)
    return true;

  const H265Pps* pps = h265_parser_->GetPps(pps_id);
  const H265Sps* sps;
  if (!pps) {
    // Only accept an invalid PPS at the beginning when the stream
    // does not necessarily start with an SPS/PPS/IDR.
    // In this case, the initial frames are conveyed to the upper layer with
    // an invalid VideoDecoderConfig and it's up to the upper layer
    // to process this kind of frame accordingly.
    return last_video_decoder_config_ == nullptr;
  } else {
    sps = h265_parser_->GetSps(pps->seq_parameter_set_id);
    if (!sps)
      return false;
    decoder_config_check_pending_ = false;
  }

  std::vector<uint8_t> decoder_config_record;
  HEVCDecoderConfigurationRecord decoder_config;
  if (!stream_converter()->GetDecoderConfigurationRecord(
          &decoder_config_record) ||
      !decoder_config.Parse(decoder_config_record)) {
    DLOG(ERROR) << "Failure to construct an HEVCDecoderConfigurationRecord";
    return false;
  }

  if (last_video_decoder_config_) {
    if (last_video_decoder_config_->codec_config() != decoder_config_record) {
      // Video configuration has changed. Issue warning.
      // TODO(tinskip): Check the nature of the configuration change. Only
      // minor configuration changes (such as frame ordering) can be handled
      // gracefully by decoders without notification. Major changes (such as
      // video resolution changes) should be treated as errors.
      LOG(WARNING) << "H.265 decoder configuration has changed.";
      last_video_decoder_config_->set_codec_config(decoder_config_record);
    }
    return true;
  }

  uint32_t coded_width = 0;
  uint32_t coded_height = 0;
  uint32_t pixel_width = 0;
  uint32_t pixel_height = 0;
  if (!ExtractResolutionFromSps(*sps, &coded_width, &coded_height, &pixel_width,
                                &pixel_height)) {
    LOG(ERROR) << "Failed to parse SPS.";
    return false;
  }

  last_video_decoder_config_ = scoped_refptr<StreamInfo>(new VideoStreamInfo(
      pid(), kMpeg2Timescale, kInfiniteDuration, kCodecHVC1,
      decoder_config.GetCodecString(kCodecHVC1), decoder_config_record.data(),
      decoder_config_record.size(), coded_width, coded_height, pixel_width,
      pixel_height, 0, H26xByteToUnitStreamConverter::kUnitStreamNaluLengthSize,
      std::string(), false));

  // Video config notification.
  new_stream_info_cb_.Run(last_video_decoder_config_);

  return true;
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka