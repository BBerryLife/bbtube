#ifndef MP4_STREAM_REMUX_HPP_
#define MP4_STREAM_REMUX_HPP_

// Public interface for the streaming MP4/ISOBMFF remuxer implemented in
// mp4_stream_remux.cpp. See that file for the full design rationale.
//
// Usage pattern (see StreamingRemuxSession.cpp for the real async version):
//   1) Fetch a small HEAD prefix (e.g. first 128KB) of the video-only and
//      audio-only source URLs (HTTP Range request).
//   2) TrackHead videoHead = parseHead(videoHeadBytes, "video");
//      TrackHead audioHead = parseHead(audioHeadBytes, "audio");
//      (parseHead throws std::runtime_error if the head buffer doesn't
//      contain a full moov + the mdat box header yet -- caller should
//      fetch a larger head and retry.)
//   3) RemuxPlan plan;
//      planStreamingRemux(videoHead, audioHead, &plan, &err);
//   4) preallocateAndWriteHead(outputPath, plan, &err);
//      -- output file now exists at its FINAL size with a valid
//      ftyp+moov+mdat-header already written.
//   5) Stream the audio body (Range: bytes=audioHead.mdatBodyOffsetInSource-)
//      and write it with writeBodyChunk(outputPath, plan.audioOutputOffset, ...).
//      Stream the video body (Range: bytes=videoHead.mdatBodyOffsetInSource-)
//      incrementally as it downloads, writing each chunk with
//      writeBodyChunk(outputPath, plan.videoOutputOffset + bytesSoFar, ...).
//
// C++03/GNU++98 only -- matches bbtube's QNX/gcc 4.6.3 toolchain. No
// `using` aliases, lambdas, non-static in-class member initializers, or
// std::initializer_list usage in this header or its .cpp.

#include <string>
#include <vector>
#include <stdint.h>

typedef std::vector<uint8_t> Mp4RemuxBytes;

// One entry from a parsed 'sidx' (segment index) box: byte range and
// duration of one fragment (a moof+mdat pair) within the source resource,
// relative to the position right after the sidx box itself.
struct SidxEntry {
    uint64_t referencedSize;   // bytes of this fragment (moof+mdat)
    uint32_t subsegmentDuration;
    SidxEntry() : referencedSize(0), subsegmentDuration(0) {}
};

// Result of parsing a 'sidx' box: where fragments start (byte offset in
// the source resource, right after the sidx box) and the list of
// fragments that follow it.
struct SidxInfo {
    size_t firstFragmentOffset; // absolute byte offset in the source resource
    uint32_t timescale;         // ticks/second for SidxEntry::subsegmentDuration
    std::vector<SidxEntry> entries;
    bool found;
    SidxInfo() : firstFragmentOffset(0), timescale(0), found(false) {}
};

// One decoded sample (from a 'trun' box) with its absolute byte offset
// and size within the source resource's mdat payload area.
struct FragSample {
    uint64_t offsetInSource; // absolute byte offset in the source resource
    uint32_t size;
    uint32_t duration;       // in the track's timescale
    // True for a sync sample (keyframe) -- decoded from this sample's
    // sample_flags (trun per-sample flags if present, else tfhd's
    // default_sample_flags, else trun's first_sample_flags for sample 0).
    // Per ISO/IEC 14496-12 8.8.3.1, bit 0x00010000 of sample_flags is
    // sample_is_non_sync_sample; a sample is a sync sample when that bit
    // is 0. Audio tracks have no concept of non-sync samples -- every
    // audio sample is always treated as sync (true) regardless of what
    // parseMoofSamples() read, since segment cut points only need to
    // land on VIDEO keyframes; splitting audio anywhere is fine.
    bool isSync;
    // trun sample_composition_time_offset (PTS - DTS, in the track's
    // timescale; signed when trun version == 1). 0 when the trun carries
    // none. Needed to rebuild 'ctts' -- without it, H.264 streams that use
    // B-frames end up with PTS == DTS in the remuxed file.
    int32_t compositionOffset;
    FragSample() : offsetInSource(0), size(0), duration(0), isSync(true),
                   compositionOffset(0) {}
};

// Metadata for one track (video-only or audio-only source),
// parsed from just the HEAD bytes of that source -- no mdat payload needed.
struct TrackHead {
    std::string label;
    bool isVideo;
    uint32_t timescale;
    uint64_t duration;

    Mp4RemuxBytes tkhdBox, mdhdBox, hdlrBox, mediaHeaderBox;
    Mp4RemuxBytes stsdBox, sttsBox, cttsBox, stscBox, stszBox, stssBox;
    bool  stcoIs64;
    Mp4RemuxBytes stcoBox;

    size_t   mdatBodyOffsetInSource; // byte offset in the ORIGINAL source resource
    uint64_t mdatDeclaredSize;       // total payload bytes, from the mdat box header

    Mp4RemuxBytes ftypBox;

    // Fragmented-mp4 (DASH adaptiveFormats) support. When isFragmented is
    // true, sttsBox/stszBox/stcoBox above are NOT populated by parseHead;
    // instead sidx is set and the caller must fetch each fragment's moof
    // header (see FragSample/parseMoofSamples) and then call
    // buildProgressiveTablesFromFragments() to populate the sample tables
    // before planStreamingRemux() is used.
    bool isFragmented;
    SidxInfo sidx;
    std::vector<FragSample> fragSamples; // filled in across all fragments, in order

    TrackHead() : isVideo(false), timescale(0), duration(0), stcoIs64(false),
                  mdatBodyOffsetInSource(0), mdatDeclaredSize(0), isFragmented(false) {}
};

// Parses ftyp/moov/mdat-header out of `headBytes` (a prefix of the source
// file/resource). Throws std::runtime_error if headBytes doesn't extend far
// enough to contain a complete moov and the mdat box header -- caller
// should fetch more bytes and retry.
TrackHead parseHead(const Mp4RemuxBytes &headBytes, const std::string &label);

// The byte-exact layout of the OUTPUT file, computable from two TrackHeads
// alone (zero mdat payload bytes needed).
struct RemuxPlan {
    Mp4RemuxBytes headBytes;     // ftyp+moov+mdat-header, write at output offset 0
    size_t   videoOutputOffset;
    uint64_t videoBodySize;      // == video TrackHead's mdatDeclaredSize
    size_t   audioOutputOffset;
    uint64_t audioBodySize;
    uint64_t totalOutputSize;    // final size to pre-allocate the output file to

    RemuxPlan() : videoOutputOffset(0), videoBodySize(0), audioOutputOffset(0),
                  audioBodySize(0), totalOutputSize(0) {}
};

// NOTE: mutates videoHead.stcoBox / audioHead.stcoBox in place (patches
// chunk offsets to their final output positions) -- pass by non-const ref.
bool planStreamingRemux(TrackHead &videoHead, TrackHead &audioHead,
                         RemuxPlan *outPlan, std::string *errorOut);

// --- Fragmented-mp4 (DASH adaptiveFormats) support -------------------------
//
// YouTube/Invidious adaptiveFormats URLs serve DASH-style fragmented mp4:
// ftyp + moov (no real sample table -- just mvex/trex defaults) + sidx +
// a sequence of (moof + mdat) fragment pairs. parseHead() detects this
// (moov's stbl has an empty stco -- 0 entries) and sets isVideo/timescale/
// duration/ftypBox as before, plus isFragmented=true and (if a sidx box is
// present in headBytes) sidx.
//
// Caller workflow for a fragmented TrackHead:
//   1) parseHead() as usual. If result.isFragmented is true, continue below;
//      the usual sttsBox/stszBox/stcoBox are left empty.
//   2) If result.sidx.found is false, the head fetch wasn't large enough to
//      reach the sidx box -- refetch with a bigger head size and retry
//      parseHead(). (sidx normally sits right after moov, so this is rare.)
//   3) For each SidxEntry in result.sidx.entries (in order), issue a small
//      Range request for that fragment's moof (NOT its mdat payload -- moof
//      is typically a few hundred bytes at the start of the fragment) and
//      call parseMoofSamples() on the bytes received, passing the
//      fragment's absolute start offset (running sum of sidx.entries[i].
//      referencedSize, starting at sidx.firstFragmentOffset) and the
//      track's timescale. Append the returned samples to
//      result.fragSamples in fragment order.
//   4) Once every fragment's samples have been collected, call
//      buildProgressiveTablesFromFragments(result) to synthesize sttsBox/
//      stszBox/stcoBox/stscBox (stcoIs64 as needed) from result.fragSamples,
//      and mdatBodyOffsetInSource/mdatDeclaredSize spanning the *first*
//      sample to the *last* sample's end. From here on the TrackHead behaves
//      exactly like a non-fragmented one for planStreamingRemux() -- except
//      the source is no longer contiguous, so body streaming must fetch each
//      fragment's mdat individually (see StreamingRemuxSession) rather than
//      a single "Range: bytes=X-" covering the whole tail.

// Parses a 'sidx' box located anywhere in headBytes[searchStart,end).
// fragmentBaseOffset is the absolute byte offset in the source resource of
// the sidx box's OWN start (needed because sidx's first_offset field, if
// nonzero, is relative to the byte right after the sidx box). Returns a
// SidxInfo with found=false if no sidx box is present in range.
SidxInfo parseSidx(const Mp4RemuxBytes &headBytes, size_t searchStart, size_t end,
                    size_t sidxBoxAbsoluteStart);

// Parses one fragment's 'moof' box (moofBytes = just that fragment's moof,
// NOT including its mdat) and returns the samples it describes, with
// offsetInSource computed as fragmentStartOffset + (this fragment's moof
// size) + (per-sample offset from trun/tfhd). fragmentStartOffset is this
// fragment's absolute byte offset in the source resource (i.e. where its
// moof begins). Throws std::runtime_error if moofBytes doesn't contain a
// complete moof/traf/trun.
std::vector<FragSample> parseMoofSamples(const Mp4RemuxBytes &moofBytes,
                                          size_t fragmentStartOffset,
                                          const std::string &label);

// Synthesizes sttsBox/stszBox/stscBox/stcoBox (and stcoIs64,
// mdatBodyOffsetInSource, mdatDeclaredSize) from track.fragSamples (must be
// non-empty and in playback order). Leaves everything else in `track`
// untouched. Throws std::runtime_error on empty fragSamples.
void buildProgressiveTablesFromFragments(TrackHead &track);

// --- Segmented playback support (HLS-style: many small standalone MP4s) ---
//
// One [startSampleIndex, endSampleIndex) half-open range into a video
// TrackHead's fragSamples, marking one playback segment. Always starts on
// a sync sample (keyframe) except possibly segment 0 if the source's very
// first sample somehow isn't sync (rare/malformed source -- handled by
// just using sample 0 as-is rather than producing an empty first segment).
struct VideoSegmentBounds {
    size_t startSampleIndex;
    size_t endSampleIndex;   // exclusive
    uint64_t startTime;      // in video track's timescale, == fragSamples[startSampleIndex]'s running start time
    uint64_t endTime;        // exclusive, same units
};

// Cuts video.fragSamples into segments of approximately targetDurationTicks
// each (in the VIDEO track's timescale -- e.g. targetDurationSeconds *
// video.timescale), with every segment boundary landing exactly on a sync
// sample (video.fragSamples[i].isSync == true). A segment only ends once
// duration-so-far reaches the target AND the next available sample is a
// sync sample; if keyframes are sparser than the target, segments will run
// longer than requested rather than split mid-GOP. The final segment
// always runs to the end of fragSamples, however short. Throws
// std::runtime_error if video.fragSamples is empty, or if
// video.fragSamples[0].isSync is false and no later sync sample exists
// (degenerate/undecodable source -- there would be no valid segment start
// anywhere).
std::vector<VideoSegmentBounds> planVideoSegments(const TrackHead &video,
                                                   uint64_t targetDurationTicks);

// Given VideoSegmentBounds (in the video track's timescale) and an audio
// TrackHead, returns the [startSampleIndex, endSampleIndex) slice of
// audio.fragSamples whose time range best covers the same wall-clock
// window as the video segment -- converting between the two tracks'
// timescales internally. Audio has no sync-sample constraint (any sample
// boundary is a valid cut point), so this simply finds the audio samples
// whose start time falls within [bounds.startTime, bounds.endTime) when
// both are expressed in seconds. The LAST audio segment (i.e. when
// bounds.endSampleIndex == video.fragSamples.size()) always extends to
// the end of audio.fragSamples, so no audio is ever dropped at the very
// end of the video due to rounding.
struct AudioSegmentBounds {
    size_t startSampleIndex;
    size_t endSampleIndex; // exclusive
};
AudioSegmentBounds matchAudioSegment(const VideoSegmentBounds &bounds, uint32_t videoTimescale,
                                      const TrackHead &video, const TrackHead &audio,
                                      bool isLastVideoSegment);

// Builds a standalone TrackHead for one segment: copies every static field
// from `source` (tkhd/mdhd/hdlr/stsd/ftyp/timescale/isVideo/label/etc) but
// replaces fragSamples with source.fragSamples[startIdx..endIdx), then
// calls buildProgressiveTablesFromFragments() on the result so its
// stts/stsz/stco/etc describe ONLY this segment's samples, addressed as if
// they started fresh (sample table offsets are still the original absolute
// source byte offsets -- planStreamingRemux() / body-streaming callers are
// unaffected by this being a sub-range). Throws std::runtime_error if
// startIdx >= endIdx or endIdx > source.fragSamples.size().
TrackHead sliceTrackHeadForSegment(const TrackHead &source, size_t startIdx, size_t endIdx);

// Creates/truncates outputPath, resizes it to plan.totalOutputSize, and
// writes plan.headBytes at offset 0. After this call the file is
// structurally valid ISOBMFF (openable/probeable) even though the mdat
// payload is still all zero bytes.
bool preallocateAndWriteHead(const std::string &outputPath, const RemuxPlan &plan,
                              std::string *errorOut);

// Writes `len` bytes from `data` at `outputOffset` in outputPath (opens,
// seeks, writes, closes). Safe to call repeatedly with increasing offsets
// as chunks of a track's body arrive over the network.
bool writeBodyChunk(const std::string &outputPath, uint64_t outputOffset,
                     const uint8_t *data, size_t len, std::string *errorOut);


// --- Partial remux (chunked / progressive playback) -----------------------
//
// Builds the complete head (ftyp + moov + mdat header) of a *valid, fully
// self-contained* MP4 that covers only the first videoSampleCount samples of
// videoSrc.fragSamples and the first audioSampleCount samples of
// audioSrc.fragSamples. Neither source is modified.
//
// Output layout is the same as planStreamingRemux(): [head][audio body]
// [video body], where "audio body" is the byte-wise concatenation of the
// first audioSampleCount samples' payloads in playback order (no gaps) and
// likewise for video. mvhd/mdhd/tkhd durations are set from the samples that
// are actually included, so the file reports its true (partial) length.
//
// Both counts must be >= 1 and <= the respective fragSamples.size().
bool planPartialRemux(const TrackHead &videoSrc, const TrackHead &audioSrc,
                       size_t videoSampleCount, size_t audioSampleCount,
                       RemuxPlan *outPlan, std::string *errorOut);

// Groups a video sidx's fragments into chunks of at least targetSeconds
// each (a chunk is a run of whole fragments; fragments are keyframe-aligned
// GOPs, so every chunk starts on a keyframe). No network access needed --
// only the sidx timing. The last chunk may be shorter. Returns an empty
// vector if sidx has no entries or no timescale.
struct ChunkPlanEntry {
    size_t fragStart;   // first fragment index (inclusive)
    size_t fragEnd;     // last fragment index (exclusive)
    double startSec;
    double endSec;
};
std::vector<ChunkPlanEntry> planChunksFromSidx(const SidxInfo &videoSidx, double targetSeconds);

// Cumulative start time (seconds) of each fragment in a sidx, plus one
// extra trailing entry == total duration. size() == entries.size() + 1.
std::vector<double> sidxFragmentStartSeconds(const SidxInfo &sidx);

// Sanity check for the bytes of H.264 samples that were fetched with
// several parallel Range requests: walks each sample's length-prefixed NAL
// units (length field size taken from the track's avcC box) and checks they
// add up to exactly the sample size. Catches a relay answering a Range
// request with the right number of bytes but the wrong bytes -- the only
// failure a plain length check cannot see. Returns true when everything is
// consistent OR when the check does not apply (non-AVC track, unknown
// length-field size); `data` is the contiguous concatenation of the
// samples in `samples[0..count)`.
bool verifyAvcSamples(const TrackHead &track, const std::vector<FragSample> &samples,
                       size_t firstIdx, size_t count, const uint8_t *data, size_t dataLen);

// Same idea for raw AAC ('mp4a') audio, where there is no NAL structure to
// walk: every raw_data_block starts with a 3-bit syntax-element id, and id 7
// (ID_END) as the very first element only ever appears in tiny (<= 2 byte)
// silent frames. Random/mixed-up bytes violate that in ~1 of 8 frames, so a
// whole chunk (dozens to hundreds of frames) of wrong data is caught with
// near certainty while genuine AAC never trips it. Returns true when the
// track is not AAC.
bool verifyAacSamples(const TrackHead &track, const std::vector<FragSample> &samples,
                       size_t firstIdx, size_t count, const uint8_t *data, size_t dataLen);

#endif /* MP4_STREAM_REMUX_HPP_ */
