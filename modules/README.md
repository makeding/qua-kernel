# Qua-specific external modules

These source files correspond to the combined GPU/video kernel profile.
`scripts/build-qua.sh` builds each against the same kernel output. The GPU
initialization, I2C and DMA-limit modules are prerequisites for the board's
Panfrost integration; `video/ve1` and `video/ion` provide the Realtek video
engine and allocator. Loading a module alone is not evidence that a complete
codec pipeline works. The tested userspace components are in
`media/qua-transcode`; no system services are included.

On the tested 1440x1080 MPEG-2 sample, CPU `yadif` at one output frame per
input frame with hardware decoding and encoding reached about 0.708x. The
vendor `libRTKVoutUtil.so` exposes `SetDeintMode` and
`libRTKVOWriteback.so` exposes `lib_rtk_codec_vo_prepareBuffer_deinterlace`
and writeback functions. These are leads for the vendor video-output/RPC
path; that vendor video-output deinterlacing path has not been verified.
The Panfrost DMA-BUF pipeline in `media/qua-transcode` separately performs
one-field GPU deinterlacing between hardware decode and encode. The older
1.21x processing result did not include deinterlacing.
