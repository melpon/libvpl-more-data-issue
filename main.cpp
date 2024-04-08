#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <vpl/mfx.h>
#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>

std::vector<uint8_t> read_frame(int frame) {
  char name[256];
  sprintf(name, "bin/frame_%04d.h264", frame);
  FILE* fp = fopen(name, "rb");
  if (fp == NULL) {
    return {};
  }
  fseek(fp, 0, SEEK_END);
  size_t file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> buf(file_size);
  fread(buf.data(), 1, buf.size(), fp);
  fclose(fp);
  return buf;
}

int main(int argc, char** argv) {
  int start_frame = 0;
  if (argc >= 2) {
    start_frame = std::stoi(argv[1]);
  }

  // init
  mfxLoader loader = MFXLoad();
  if (loader == nullptr) {
    fprintf(stderr, "Failed to MFXLoad\n");
    return -1;
  }
  MFX_ADD_PROPERTY_U32(loader, "mfxImplDescription.Impl",
                       MFX_IMPL_TYPE_HARDWARE);

  mfxStatus sts = MFX_ERR_NONE;
  mfxSession session = nullptr;

#define CHECK(expr)                                      \
  sts = expr;                                            \
  if (sts != MFX_ERR_NONE) {                             \
    fprintf(stderr, "Failed to " #expr ": sts=%d", sts); \
    return -1;                                           \
  }

  CHECK(MFXCreateSession(loader, 0, &session));

  mfxVideoParam param;
  memset(&param, 0, sizeof(param));

  param.mfx.CodecId = MFX_CODEC_AVC;
  param.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
  param.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
  param.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
  param.mfx.FrameInfo.Width = 2048;
  param.mfx.FrameInfo.Height = 2048;
  param.AsyncDepth = 1;
  param.IOPattern = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

  CHECK(MFXVideoDECODE_Query(session, &param, &param));
  mfxFrameAllocRequest alloc;
  memset(&alloc, 0, sizeof(alloc));
  CHECK(MFXVideoDECODE_QueryIOSurf(session, &param, &alloc));
  CHECK(MFXVideoDECODE_Init(session, &param));

  std::vector<mfxFrameSurface1> surfaces;
  std::vector<uint8_t> surface_buffer;
  {
    int width = alloc.Info.Width;
    int height = alloc.Info.Height;
    int size = width * height * 12 / 8;
    surface_buffer.resize(alloc.NumFrameSuggested * size);

    surfaces.clear();
    surfaces.reserve(alloc.NumFrameSuggested);
    for (int i = 0; i < alloc.NumFrameSuggested; i++) {
      mfxFrameSurface1 surface;
      memset(&surface, 0, sizeof(surface));
      surface.Info = param.mfx.FrameInfo;
      surface.Data.Y = surface_buffer.data() + i * size;
      surface.Data.UV = surface_buffer.data() + i * size + width * height;
      surface.Data.Pitch = width;
      surfaces.push_back(surface);
    }
  }

  // decode
  int total_decoded = 0;
  for (int frame = start_frame;; frame++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(33));
    auto image = read_frame(frame);
    if (image.empty()) {
      // loop
      frame = 0;
      image = read_frame(frame);
    }
    mfxBitstream bs;
    memset(&bs, 0, sizeof(bs));
    bs.Data = image.data();
    bs.DataLength = image.size();
    bs.MaxLength = image.size();

    auto surface =
        std::find_if(surfaces.begin(), surfaces.end(),
                     [](const mfxFrameSurface1& s) { return !s.Data.Locked; });
    assert(surface != surfaces.end());

    bool decoded = false;
    while (true) {
      mfxSyncPoint syncp;
      mfxFrameSurface1* out_surface = nullptr;

      while (true) {
        sts = MFXVideoDECODE_DecodeFrameAsync(session, &bs, &*surface,
                                              &out_surface, &syncp);
        if (sts == MFX_WRN_DEVICE_BUSY) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        break;
      }

      if (sts == MFX_ERR_MORE_DATA) {
        if (!decoded) {
          printf("MFX_ERR_MORE_DATA: frame=%d decoded=%d\n", frame,
                 total_decoded);
        }
        break;
      }
      if (!syncp) {
        fprintf(stderr, "Failed to DecodeFrameAsync: syncp is null, sts=%d\n",
                (int)sts);
        continue;
      }
      CHECK(MFXVideoCORE_SyncOperation(session, syncp, 600000));
      total_decoded += 1;
      decoded = true;

      mfxVideoParam param;
      memset(&param, 0, sizeof(param));
      CHECK(MFXVideoDECODE_GetVideoParam(session, &param));

      int width = param.mfx.FrameInfo.CropW;
      int height = param.mfx.FrameInfo.CropH;

      printf("Decoded frame:     frame=%d decoded=%d (%dx%d)\n", frame,
             total_decoded, width, height);
    }
  }
}