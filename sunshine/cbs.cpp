extern "C" {
#include <cbs/cbs_h264.h>
#include <cbs/cbs_h265.h>
#include <cbs/h264_levels.h>
#include <libavcodec/avcodec.h>
}

#include "main.h"
#include "utility.h"

using namespace std::literals;
namespace cbs {
void close(CodedBitstreamContext *c) {
  ff_cbs_close(&c);
}

using ctx_t = util::safe_ptr<CodedBitstreamContext, close>;

class frag_t : public CodedBitstreamFragment {
public:
  frag_t(frag_t &&o) {
    std::copy((std::uint8_t *)&o, (std::uint8_t *)(&o + 1), (std::uint8_t *)this);

    o.data  = nullptr;
    o.units = nullptr;
  };

  frag_t() {
    std::fill_n((std::uint8_t *)this, sizeof(*this), 0);
  }

  frag_t &operator=(frag_t &&o) {
    std::copy((std::uint8_t *)&o, (std::uint8_t *)(&o + 1), (std::uint8_t *)this);

    o.data  = nullptr;
    o.units = nullptr;

    return *this;
  };


  ~frag_t() {
    if(data || units) {
      ff_cbs_fragment_free(this);
    }
  }
};

util::buffer_t<std::uint8_t> write(const H264RawNALUnitHeader &uh, AVCodecID codec_id) {
  cbs::frag_t frag;
  auto err = ff_cbs_insert_unit_content(&frag, -1, uh.nal_unit_type, (void *)&uh, nullptr);
  if(err < 0) {
    char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
    BOOST_LOG(error) << "Could not NAL unit SPS: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

    return {};
  }

  cbs::ctx_t cbs_ctx;
  ff_cbs_init(&cbs_ctx, codec_id, nullptr);

  err = ff_cbs_write_fragment_data(cbs_ctx.get(), &frag);
  if(err < 0) {
    char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
    BOOST_LOG(error) << "Could not write fragment data: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

    return {};
  }

  // frag.data_size * 8 - frag.data_bit_padding == bits in fragment
  util::buffer_t<std::uint8_t> data { frag.data_size };
  std::copy_n(frag.data, frag.data_size, std::begin(data));

  return data;
}

util::buffer_t<std::uint8_t> make_sps_h264(const AVCodecContext *ctx) {
  H264RawSPS sps {};

  /* b_per_p == ctx->max_b_frames for h264 */
  /* desired_b_depth == avoption("b_depth") == 1 */
  /* max_b_depth == std::min(av_log2(ctx->b_per_p) + 1, desired_b_depth) ==> 1 */
  auto max_b_depth = 1;
  auto dpb_frame   = ctx->gop_size == 1 ? 0 : 1 + max_b_depth;
  auto mb_width    = (FFALIGN(ctx->width, 16) / 16) * 16;
  auto mb_height   = (FFALIGN(ctx->height, 16) / 16) * 16;


  sps.nal_unit_header.nal_ref_idc   = 3;
  sps.nal_unit_header.nal_unit_type = H264_NAL_SPS;

  sps.profile_idc = FF_PROFILE_H264_HIGH & 0xFF;

  sps.constraint_set1_flag = 1;

  if(ctx->level != FF_LEVEL_UNKNOWN) {
    sps.level_idc = ctx->level;
  }
  else {
    auto framerate = ctx->framerate;

    auto level = ff_h264_guess_level(
      sps.profile_idc,
      ctx->bit_rate,
      framerate.num / framerate.den,
      mb_width,
      mb_height,
      dpb_frame);

    if(!level) {
      BOOST_LOG(error) << "Could not guess h264 level"sv;

      return {};
    }
    sps.level_idc = level->level_idc;
  }

  sps.seq_parameter_set_id = 0;
  sps.chroma_format_idc    = 1;

  sps.log2_max_frame_num_minus4         = 3; //4;
  sps.pic_order_cnt_type                = 0;
  sps.log2_max_pic_order_cnt_lsb_minus4 = 0; //4;

  sps.max_num_ref_frames = dpb_frame;

  sps.pic_width_in_mbs_minus1        = mb_width / 16 - 1;
  sps.pic_height_in_map_units_minus1 = mb_height / 16 - 1;

  sps.frame_mbs_only_flag       = 1;
  sps.direct_8x8_inference_flag = 1;

  if(ctx->width != mb_width || ctx->height != mb_height) {
    sps.frame_cropping_flag      = 1;
    sps.frame_crop_left_offset   = 0;
    sps.frame_crop_top_offset    = 0;
    sps.frame_crop_right_offset  = (mb_width - ctx->width) / 2;
    sps.frame_crop_bottom_offset = (mb_height - ctx->height) / 2;
  }

  sps.vui_parameters_present_flag = 1;

  auto &vui = sps.vui;

  vui.video_format                    = 5;
  vui.colour_description_present_flag = 1;
  vui.video_signal_type_present_flag  = 1;
  vui.video_full_range_flag           = ctx->color_range == AVCOL_RANGE_JPEG;
  vui.colour_primaries                = ctx->color_primaries;
  vui.transfer_characteristics        = ctx->color_trc;
  vui.matrix_coefficients             = ctx->colorspace;

  vui.low_delay_hrd_flag = 1 - vui.fixed_frame_rate_flag;

  vui.bitstream_restriction_flag              = 1;
  vui.motion_vectors_over_pic_boundaries_flag = 1;
  vui.log2_max_mv_length_horizontal           = 15;
  vui.log2_max_mv_length_vertical             = 15;
  vui.max_num_reorder_frames                  = max_b_depth;
  vui.max_dec_frame_buffering                 = max_b_depth + 1;

  return write(sps.nal_unit_header, AV_CODEC_ID_H264);
}

util::buffer_t<std::uint8_t> read_sps(const AVPacket *packet, int codec_id) {
  cbs::ctx_t ctx;
  if(ff_cbs_init(&ctx, (AVCodecID)codec_id, nullptr)) {
    return {};
  }

  cbs::frag_t frag;

  int err = ff_cbs_read_packet(ctx.get(), &frag, &*packet);
  if(err < 0) {
    char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
    BOOST_LOG(error) << "Couldn't read packet: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

    return {};
  }

  H264RawNALUnitHeader *p;
  if(codec_id == AV_CODEC_ID_H264) {
    p = (H264RawNALUnitHeader *)((CodedBitstreamH264Context *)ctx->priv_data)->active_sps;
  }
  else {
    p = (H264RawNALUnitHeader *)((CodedBitstreamH265Context *)ctx->priv_data)->active_sps;
  }

  return write(*p, (AVCodecID)codec_id);
}

util::buffer_t<std::uint8_t> make_sps(const AVCodecContext *ctx, int format) {
  switch(format) {
  case 0:
    return make_sps_h264(ctx);
  }

  BOOST_LOG(warning) << "make_sps: video format ["sv << format << "] not supported"sv;

  return {};
}

bool validate_sps(const AVPacket *packet, int codec_id) {
  cbs::ctx_t ctx;
  if(ff_cbs_init(&ctx, (AVCodecID)codec_id, nullptr)) {
    return false;
  }

  cbs::frag_t frag;

  int err = ff_cbs_read_packet(ctx.get(), &frag, &*packet);
  if(err < 0) {
    char err_str[AV_ERROR_MAX_STRING_SIZE] { 0 };
    BOOST_LOG(error) << "Couldn't read packet: "sv << av_make_error_string(err_str, AV_ERROR_MAX_STRING_SIZE, err);

    return false;
  }

  if(codec_id == AV_CODEC_ID_H264) {
    auto h264 = (CodedBitstreamH264Context *)ctx->priv_data;

    if(!h264->active_sps->vui_parameters_present_flag) {
      return false;
    }

    return true;
  }

  return ((CodedBitstreamH265Context *)ctx->priv_data)->active_sps->vui_parameters_present_flag;
}
} // namespace cbs