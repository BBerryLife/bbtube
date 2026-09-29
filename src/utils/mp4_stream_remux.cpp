// mp4_stream_remux.cpp
//
// STREAMING MP4/ISOBMFF remuxer -- see mp4_stream_remux.hpp for the public
// interface and usage pattern, and the design-rationale comment block
// there. Short version: YouTube's adaptiveFormats mp4s put the moov box
// (full sample table) at the FRONT of the file, so the entire output
// layout can be computed from a small HEAD fetch (typically <1% of the
// file), before any sample payload has been downloaded. That lets us
// pre-allocate the final output file and stream both tracks' payload
// directly into their final byte positions as the network delivers them --
// no "wait for both downloads to finish, then remux" step.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain): no
// `using` aliases, lambdas, non-static in-class member initializers, or
// std::initializer_list usage anywhere in this file.

#include "mp4_stream_remux.hpp"

#include <algorithm>
#include <cstring>
#include <stdio.h>
#include <fstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Low level big-endian helpers
// ---------------------------------------------------------------------------
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t hi = rd32(p);
    uint64_t lo = rd32(p + 4);
    return (hi << 32) | lo;
}
static void wr32(Mp4RemuxBytes &out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
static std::string fourccStr(const uint8_t *p) {
    return std::string(reinterpret_cast<const char *>(p), 4);
}
static void append(Mp4RemuxBytes &dst, const Mp4RemuxBytes &src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

// ---------------------------------------------------------------------------
// Box location within a byte range
// ---------------------------------------------------------------------------
struct BoxLoc {
    size_t headerOffset;
    size_t payloadOffset;
    size_t payloadSize;
    size_t totalSize;
    bool found;
    BoxLoc() : headerOffset(0), payloadOffset(0), payloadSize(0), totalSize(0), found(false) {}
};

static BoxLoc findBox(const Mp4RemuxBytes &data, size_t start, size_t end, const std::string &type,
                       bool allowPayloadBeyondEnd = false);
static BoxLoc findBoxOrThrow(const Mp4RemuxBytes &data, size_t start, size_t end,
                              const std::string &sourceLabel, const char *boxName,
                              bool allowPayloadBeyondEnd = false);
static Mp4RemuxBytes makeBox(const std::string &type, const Mp4RemuxBytes &payload);

static BoxLoc findBox(const Mp4RemuxBytes &data, size_t start, size_t end, const std::string &type,
                       bool allowPayloadBeyondEnd) {
    size_t pos = start;
    while (pos + 8 <= end) {
        uint32_t sz32 = rd32(&data[pos]);
        std::string t = fourccStr(&data[pos + 4]);
        uint64_t boxSize = sz32;
        size_t headerLen = 8;
        if (sz32 == 1) {
            if (pos + 16 > end) break;
            boxSize = rd64(&data[pos + 8]);
            headerLen = 16;
        } else if (sz32 == 0) {
            // "extends to EOF" -- total size unknown until fully
            // downloaded. YouTube's faststart transcoder output always
            // writes a definite mdat size, so this shouldn't occur in
            // practice; fail loudly rather than guess.
            break;
        }
        if (boxSize < headerLen) break; // genuinely malformed
        if (pos + boxSize > end) {
            // This box's declared payload extends beyond what we've
            // fetched so far -- EXPECTED for mdat in a head-only buffer.
            // Return it (header-only info) if it's the box being searched
            // for; otherwise we can't safely skip past it to find
            // siblings, so stop scanning.
            if (t == type && allowPayloadBeyondEnd) {
                BoxLoc loc;
                loc.headerOffset = pos;
                loc.payloadOffset = pos + headerLen;
                loc.payloadSize = size_t(boxSize) - headerLen;
                loc.totalSize = size_t(boxSize);
                loc.found = true;
                return loc;
            }
            break;
        }
        if (t == type) {
            BoxLoc loc;
            loc.headerOffset = pos;
            loc.payloadOffset = pos + headerLen;
            loc.payloadSize = size_t(boxSize) - headerLen;
            loc.totalSize = size_t(boxSize);
            loc.found = true;
            return loc;
        }
        pos += size_t(boxSize);
    }
    return BoxLoc();
}

static Mp4RemuxBytes sliceBox(const Mp4RemuxBytes &data, const BoxLoc &loc) {
    return Mp4RemuxBytes(data.begin() + loc.headerOffset, data.begin() + loc.headerOffset + loc.totalSize);
}

// ---------------------------------------------------------------------------
// Fragmented-mp4 (sidx/moof/traf/trun) parsing -- see header for the
// caller-facing workflow. ISO/IEC 14496-12 box layouts referenced below.
// ---------------------------------------------------------------------------

// sidx (Segment Index Box), ISO/IEC 14496-12 �8.16.3:
//   version(1) flags(3) reference_ID(4) timescale(4)
//   version==0: earliest_presentation_time(4) first_offset(4)
//   version==1: earliest_presentation_time(8) first_offset(8)
//   reserved(2) reference_count(2)
//   reference_count * { reference_type+referenced_size(4)
//                        subsegment_duration(4)
//                        starts_with_SAP+SAP_type+SAP_delta_time(4) }
SidxInfo parseSidx(const Mp4RemuxBytes &headBytes, size_t searchStart, size_t end,
                    size_t sidxBoxAbsoluteStart) {
    SidxInfo info;
    BoxLoc sidx = findBox(headBytes, searchStart, end, "sidx");
    if (!sidx.found) return info;

    const uint8_t *p = &headBytes[sidx.payloadOffset];
    size_t payloadLen = sidx.payloadSize;
    if (payloadLen < 12) return info; // malformed/truncated

    uint8_t version = p[0];
    size_t off = 4; // skip version+flags
    off += 4;        // reference_ID
    off += 4;        // timescale
    uint64_t firstOffset;
    if (version == 0) {
        if (payloadLen < off + 8 + 4) return info;
        off += 4; // earliest_presentation_time
        firstOffset = rd32(p + off);
        off += 4;
    } else {
        if (payloadLen < off + 16 + 4) return info;
        off += 8; // earliest_presentation_time
        firstOffset = rd64(p + off);
        off += 8;
    }
    if (payloadLen < off + 4) return info;
    off += 2; // reserved
    uint16_t referenceCount = uint16_t((uint16_t(p[off]) << 8) | uint16_t(p[off + 1]));
    off += 2;

    // The sidx box's payload ends at sidxBoxAbsoluteStart + sidx.totalSize
    // (header+payload). first_offset is relative to that point.
    info.firstFragmentOffset = sidxBoxAbsoluteStart + sidx.totalSize + size_t(firstOffset);

    for (uint16_t i = 0; i < referenceCount; i++) {
        if (off + 12 > payloadLen) break; // truncated -- return what we have
        uint32_t word0 = rd32(p + off);
        SidxEntry e;
        e.referencedSize = word0 & 0x7FFFFFFFu; // top bit is reference_type, ignore
        e.subsegmentDuration = rd32(p + off + 4);
        info.entries.push_back(e);
        off += 12;
    }
    info.found = true;
    return info;
}

// moof (Movie Fragment Box) -> traf (Track Fragment Box) -> tfhd (Track
// Fragment Header) + trun (Track Fragment Run), ISO/IEC 14496-12 �8.8.
//
// tfhd flags bits we care about (low 24 bits of the 4-byte flags field):
//   0x000001 base-data-offset-present
//   0x000002 sample-description-index-present
//   0x000008 default-sample-duration-present
//   0x000010 default-sample-size-present
//   0x000020 default-sample-flags-present
//
// trun flags bits we care about:
//   0x000001 data-offset-present
//   0x000004 first-sample-flags-present
//   0x000100 sample-duration-present
//   0x000200 sample-size-present
//   0x000400 sample-flags-present
//   0x000800 sample-composition-time-offsets-present
std::vector<FragSample> parseMoofSamples(const Mp4RemuxBytes &moofBytes,
                                          size_t fragmentStartOffset,
                                          const std::string &label) {
    std::vector<FragSample> out;

    BoxLoc moof = findBoxOrThrow(moofBytes, 0, moofBytes.size(), label, "moof");
    size_t moofEnd = moof.payloadOffset + moof.payloadSize;

    BoxLoc traf = findBoxOrThrow(moofBytes, moof.payloadOffset, moofEnd, label, "traf");
    size_t trafStart = traf.payloadOffset, trafEnd = traf.payloadOffset + traf.payloadSize;

    BoxLoc tfhd = findBoxOrThrow(moofBytes, trafStart, trafEnd, label, "tfhd");
    const uint8_t *tp = &moofBytes[tfhd.payloadOffset];
    uint32_t tfhdFlags = rd32(tp) & 0x00FFFFFFu; // low 24 bits; byte0 is version
    size_t tOff = 4;
    tOff += 4; // track_ID
    bool haveValidBaseDataOffset = false;
    uint64_t baseDataOffset = 0;
    if (tfhdFlags & 0x000001) {
        baseDataOffset = rd64(tp + tOff);
        tOff += 8;
        // Some muxers write base_data_offset==0 as a de facto "unset"
        // placeholder even though bit 0x000001 is set (seen in the wild
        // on YouTube/Invidious adaptiveFormats: the first fragment of a
        // video track can have this, while later fragments and the audio
        // track use a valid absolute offset or omit the flag entirely).
        // An absolute byte offset of exactly 0 into a resource that
        // starts with ftyp/moov/sidx is never actually valid for a media
        // sample, so treat it the same as the flag being absent below
        // (default-base-is-moof) rather than trusting it.
        haveValidBaseDataOffset = (baseDataOffset != 0);
    }
    if (tfhdFlags & 0x000002) { tOff += 4; } // sample_description_index
    uint32_t defaultSampleDuration = 0;
    if (tfhdFlags & 0x000008) { defaultSampleDuration = rd32(tp + tOff); tOff += 4; }
    uint32_t defaultSampleSize = 0;
    if (tfhdFlags & 0x000010) { defaultSampleSize = rd32(tp + tOff); tOff += 4; }
    // sample_is_non_sync_sample is bit 0x00010000 of a sample_flags word;
    // a sample is a sync sample (keyframe) when that bit is CLEAR. Kept as
    // "defaultSampleIsSync" (already inverted) so every place below just
    // ORs bits together without re-deriving the polarity each time.
    bool defaultSampleIsSync = true; // used only if tfhd sets default_sample_flags
    if (tfhdFlags & 0x000020) {
        uint32_t defaultSampleFlags = rd32(tp + tOff);
        defaultSampleIsSync = (defaultSampleFlags & 0x00010000u) == 0;
        tOff += 4;
    }

    // moof's total byte length -- needed because trun's data_offset (when
    // data-offset-present) is relative to the moof's start, and the mdat
    // payload for this fragment begins right after the moof box ends
    // (fragmentStartOffset + moof.totalSize), i.e. that's the base if no
    // base-data-offset was given in tfhd.
    size_t moofTotalSize = moof.headerOffset == 0 ? moof.payloadOffset + moof.payloadSize
                                                   : moof.payloadOffset + moof.payloadSize - moof.headerOffset;
    // moof.headerOffset is 0 here since moofBytes starts at the moof box.
    (void)moofTotalSize;
    size_t mdatStartForFragment = fragmentStartOffset + (moof.payloadOffset + moof.payloadSize);

    BoxLoc trun = findBoxOrThrow(moofBytes, trafStart, trafEnd, label, "trun");
    const uint8_t *rp = &moofBytes[trun.payloadOffset];
    uint32_t trunFlags = rd32(rp) & 0x00FFFFFFu;
    size_t rOff = 4;
    uint32_t sampleCount = rd32(rp + rOff);
    rOff += 4;
    int64_t dataOffset = 0;
    if (trunFlags & 0x000001) { dataOffset = int32_t(rd32(rp + rOff)); rOff += 4; }
    bool haveFirstSampleFlags = false;
    bool firstSampleIsSync = true;
    if (trunFlags & 0x000004) {
        uint32_t firstSampleFlags = rd32(rp + rOff);
        firstSampleIsSync = (firstSampleFlags & 0x00010000u) == 0;
        haveFirstSampleFlags = true;
        rOff += 4;
    }

    // Running byte cursor for this trun's samples. If data-offset-present,
    // it's relative to the moof's start (i.e. fragmentStartOffset); else
    // if tfhd gave a valid (non-zero) base-data-offset, samples start
    // there; otherwise fall back to right after the moof box
    // (default-base-is-moof, per spec, also used when tfhd's
    // base-data-offset was present but read as the invalid placeholder 0).
    uint64_t sampleCursor;
    if (trunFlags & 0x000001) {
        sampleCursor = uint64_t(int64_t(fragmentStartOffset) + dataOffset);
    } else if (haveValidBaseDataOffset) {
        sampleCursor = baseDataOffset;
    } else {
        sampleCursor = mdatStartForFragment;
    }

    out.reserve(sampleCount);
    for (uint32_t i = 0; i < sampleCount; i++) {
        FragSample s;
        uint32_t sampleDuration = defaultSampleDuration;
        uint32_t sampleSize = defaultSampleSize;
        if (trunFlags & 0x000100) { sampleDuration = rd32(rp + rOff); rOff += 4; }
        if (trunFlags & 0x000200) { sampleSize = rd32(rp + rOff); rOff += 4; }
        bool sampleIsSync = defaultSampleIsSync; // tfhd default, if any (else true)
        if (trunFlags & 0x000400) {
            // Per-sample flags present: this OVERRIDES first_sample_flags
            // and the tfhd default for this sample, per spec.
            uint32_t sampleFlags = rd32(rp + rOff);
            sampleIsSync = (sampleFlags & 0x00010000u) == 0;
            rOff += 4;
        } else if (i == 0 && haveFirstSampleFlags) {
            // first_sample_flags only applies when trun does NOT also
            // carry per-sample flags (the common case: encoders use
            // first_sample_flags to mark sample 0 as sync while every
            // other sample in the trun defaults to non-sync).
            sampleIsSync = firstSampleIsSync;
        }
        if (trunFlags & 0x000800) { rOff += 4; } // sample_composition_time_offset

        s.offsetInSource = sampleCursor;
        s.size = sampleSize;
        s.duration = sampleDuration;
        s.isSync = sampleIsSync;
        out.push_back(s);

        sampleCursor += sampleSize;
    }
    return out;
}

void buildProgressiveTablesFromFragments(TrackHead &track) {
    if (track.fragSamples.empty())
        throw std::runtime_error(track.label + ": buildProgressiveTablesFromFragments called with no samples");

    const std::vector<FragSample> &samples = track.fragSamples;

    // stts: sample count/duration run-length pairs.
    Mp4RemuxBytes stts;
    wr32(stts, 0); // version+flags
    size_t sttsCountPos = stts.size();
    wr32(stts, 0); // entry_count, patched below
    uint32_t sttsEntryCount = 0;
    {
        size_t i = 0;
        while (i < samples.size()) {
            uint32_t dur = samples[i].duration;
            size_t runStart = i;
            while (i < samples.size() && samples[i].duration == dur) i++;
            wr32(stts, uint32_t(i - runStart));
            wr32(stts, dur);
            sttsEntryCount++;
        }
    }
    stts[sttsCountPos+0]=uint8_t(sttsEntryCount>>24); stts[sttsCountPos+1]=uint8_t(sttsEntryCount>>16);
    stts[sttsCountPos+2]=uint8_t(sttsEntryCount>>8);  stts[sttsCountPos+3]=uint8_t(sttsEntryCount);
    track.sttsBox = makeBox("stts", stts);

    // stsz: per-sample sizes (all samples explicit -- sample_size field is 0).
    Mp4RemuxBytes stsz;
    wr32(stsz, 0); // version+flags
    wr32(stsz, 0); // sample_size == 0 => explicit table below
    wr32(stsz, uint32_t(samples.size()));
    for (size_t i = 0; i < samples.size(); i++) wr32(stsz, samples[i].size);
    track.stszBox = makeBox("stsz", stsz);

    // stsc: one chunk per sample (simplest correct mapping -- chunk i has
    // exactly 1 sample, sample_description_index 1).
    Mp4RemuxBytes stsc;
    wr32(stsc, 0);
    wr32(stsc, 1); // entry_count
    wr32(stsc, 1); // first_chunk
    wr32(stsc, 1); // samples_per_chunk
    wr32(stsc, 1); // sample_description_index
    track.stscBox = makeBox("stsc", stsc);

    // stco/co64: one chunk offset per sample. Use co64 if any offset
    // exceeds 32 bits (large/long videos), else stco.
    uint64_t maxOffset = 0;
    for (size_t i = 0; i < samples.size(); i++)
        if (samples[i].offsetInSource > maxOffset) maxOffset = samples[i].offsetInSource;
    track.stcoIs64 = maxOffset > 0xFFFFFFFFu;

    Mp4RemuxBytes stco;
    wr32(stco, 0);
    wr32(stco, uint32_t(samples.size()));
    for (size_t i = 0; i < samples.size(); i++) {
        if (track.stcoIs64) {
            uint64_t v = samples[i].offsetInSource;
            stco.push_back(uint8_t(v>>56)); stco.push_back(uint8_t(v>>48));
            stco.push_back(uint8_t(v>>40)); stco.push_back(uint8_t(v>>32));
            stco.push_back(uint8_t(v>>24)); stco.push_back(uint8_t(v>>16));
            stco.push_back(uint8_t(v>>8));  stco.push_back(uint8_t(v));
        } else {
            wr32(stco, uint32_t(samples[i].offsetInSource));
        }
    }
    track.stcoBox = makeBox(track.stcoIs64 ? "co64" : "stco", stco);

    // The "mdat" for a fragmented source isn't contiguous in the original
    // resource, but mdatDeclaredSize is used elsewhere only to size the
    // OUTPUT mdat span for this track, which body-streaming now fills by
    // copying each fragment's real mdat bytes into their per-sample output
    // positions (see StreamingRemuxSession) -- so this is the sum of all
    // sample sizes, not a source byte range.
    uint64_t total = 0;
    for (size_t i = 0; i < samples.size(); i++) total += samples[i].size;
    track.mdatDeclaredSize = total;
    track.mdatBodyOffsetInSource = size_t(samples[0].offsetInSource);
}

// --- Segmented playback support ---------------------------------------

std::vector<VideoSegmentBounds> planVideoSegments(const TrackHead &video,
                                                   uint64_t targetDurationTicks) {
    if (video.fragSamples.empty())
        throw std::runtime_error(video.label + ": planVideoSegments called with no samples");

    const std::vector<FragSample> &samples = video.fragSamples;

    // Running start time of each sample, in the video track's timescale.
    // Computed once up front rather than repeatedly summing durations
    // inside the segment-boundary search below.
    std::vector<uint64_t> startTimes(samples.size());
    uint64_t t = 0;
    for (size_t i = 0; i < samples.size(); i++) {
        startTimes[i] = t;
        t += samples[i].duration;
    }
    uint64_t totalDuration = t;

    std::vector<VideoSegmentBounds> segments;
    size_t segStart = 0;
    if (!samples[segStart].isSync) {
        // Malformed/unusual source whose very first sample isn't marked
        // sync. Look for the first real sync sample and start there --
        // there is no valid way to build a standalone, decodable segment
        // beginning before a keyframe, so any leading non-sync samples
        // are simply unplayable and dropped from segment 0.
        size_t firstSync = segStart;
        while (firstSync < samples.size() && !samples[firstSync].isSync) firstSync++;
        if (firstSync >= samples.size())
            throw std::runtime_error(video.label +
                    ": planVideoSegments found no sync sample anywhere in the track");
        segStart = firstSync;
    }

    while (segStart < samples.size()) {
        uint64_t segStartTime = startTimes[segStart];
        uint64_t targetEndTime = segStartTime + targetDurationTicks;

        // Advance until duration-so-far reaches the target, then keep
        // advancing (if needed) to the next sync sample -- segments must
        // never end mid-GOP, so overshoot the target rather than cut
        // before a keyframe.
        size_t i = segStart + 1;
        while (i < samples.size() && startTimes[i] < targetEndTime) i++;
        while (i < samples.size() && !samples[i].isSync) i++;
        // i now points at either: the first sync sample at/after the
        // target duration, or samples.size() (this is the final segment).

        VideoSegmentBounds b;
        b.startSampleIndex = segStart;
        b.endSampleIndex = i;
        b.startTime = segStartTime;
        b.endTime = (i < samples.size()) ? startTimes[i] : totalDuration;
        segments.push_back(b);

        segStart = i;
    }
    return segments;
}

AudioSegmentBounds matchAudioSegment(const VideoSegmentBounds &bounds, uint32_t videoTimescale,
                                      const TrackHead &video, const TrackHead &audio,
                                      bool isLastVideoSegment) {
    (void)video; // kept in the signature for symmetry/future use; unused today
    if (audio.fragSamples.empty())
        throw std::runtime_error(audio.label + ": matchAudioSegment called with no samples");
    if (audio.timescale == 0 || videoTimescale == 0)
        throw std::runtime_error(audio.label + ": matchAudioSegment given a zero timescale");

    const std::vector<FragSample> &aSamples = audio.fragSamples;

    // Convert the video segment's [startTime, endTime) -- in the VIDEO
    // timescale -- into the AUDIO timescale via seconds, so the two
    // tracks' sample tables (which almost never share a timescale; e.g.
    // 30000 for video vs 48000 for audio) line up on wall-clock time
    // rather than raw tick counts.
    double startSeconds = double(bounds.startTime) / double(videoTimescale);
    double endSeconds   = double(bounds.endTime)   / double(videoTimescale);
    uint64_t audioStartTicks = uint64_t(startSeconds * double(audio.timescale) + 0.5);
    uint64_t audioEndTicks   = uint64_t(endSeconds   * double(audio.timescale) + 0.5);

    // Running audio start times, same approach as planVideoSegments.
    std::vector<uint64_t> startTimes(aSamples.size());
    uint64_t t = 0;
    for (size_t i = 0; i < aSamples.size(); i++) {
        startTimes[i] = t;
        t += aSamples[i].duration;
    }

    AudioSegmentBounds out;
    out.startSampleIndex = 0;
    while (out.startSampleIndex < aSamples.size()
            && startTimes[out.startSampleIndex] < audioStartTicks) {
        out.startSampleIndex++;
    }
    if (isLastVideoSegment) {
        out.endSampleIndex = aSamples.size();
    } else {
        out.endSampleIndex = out.startSampleIndex;
        while (out.endSampleIndex < aSamples.size()
                && startTimes[out.endSampleIndex] < audioEndTicks) {
            out.endSampleIndex++;
        }
        // Degenerate case: rounding put start==end (segment shorter than
        // one audio sample, extremely unlikely at ~5s target durations
        // but guarded against so no segment ever ships with zero audio
        // samples and silently desyncs). Take at least one sample.
        if (out.endSampleIndex == out.startSampleIndex && out.startSampleIndex < aSamples.size())
            out.endSampleIndex = out.startSampleIndex + 1;
    }
    return out;
}

TrackHead sliceTrackHeadForSegment(const TrackHead &source, size_t startIdx, size_t endIdx) {
    if (startIdx >= endIdx)
        throw std::runtime_error(source.label + ": sliceTrackHeadForSegment given an empty range");
    if (endIdx > source.fragSamples.size())
        throw std::runtime_error(source.label + ": sliceTrackHeadForSegment range exceeds fragSamples");

    // Copy every static field verbatim -- these describe the CODEC/track
    // identity and are identical for every segment of the same source
    // track, only the sample-table boxes (rebuilt below) and mdat sizing
    // differ per segment.
    TrackHead seg = source;
    seg.fragSamples.assign(source.fragSamples.begin() + startIdx,
                            source.fragSamples.begin() + endIdx);

    buildProgressiveTablesFromFragments(seg);
    return seg;
}

// Rebuilds an ftyp box, swapping the major_brand to "isom" (the standard
// progressive-mp4 brand) and dropping "dash" from the compatible_brands
// list. Sources fetched from YouTube/Invidious adaptiveFormats are DASH
// init segments whose ftyp has major_brand == "dash" -- copying that box
// verbatim into a plain (non-fragmented) output file causes BB10's
// mmrenderer to reject the file outright (it fails to match any input
// plugin for that brand and falls through playlist/autolist detection to
// UnsupportedMediaType) even though the rest of the container is a
// perfectly valid single-moov/single-mdat mp4. The compatible_brands
// entries (isom/iso6/avc1/mp41 etc.) are preserved as-is minus "dash".
static Mp4RemuxBytes rebuildFtypBox(const Mp4RemuxBytes &srcFtypBox) {
    // srcFtypBox layout: size(4) + "ftyp"(4) + major_brand(4) + minor_version(4)
    // + compatible_brands(4 each, repeated).
    if (srcFtypBox.size() < 16) {
        // Malformed/too-short ftyp -- fall back to a minimal standard one.
        Mp4RemuxBytes payload;
        payload.insert(payload.end(), 'i'); payload.insert(payload.end(), 's');
        payload.insert(payload.end(), 'o'); payload.insert(payload.end(), 'm');
        wr32(payload, 0); // minor_version
        payload.insert(payload.end(), 'i'); payload.insert(payload.end(), 's');
        payload.insert(payload.end(), 'o'); payload.insert(payload.end(), 'm');
        payload.insert(payload.end(), 'm'); payload.insert(payload.end(), 'p');
        payload.insert(payload.end(), '4'); payload.insert(payload.end(), '1');
        Mp4RemuxBytes out;
        wr32(out, uint32_t(8 + payload.size()));
        out.insert(out.end(), 'f'); out.insert(out.end(), 't');
        out.insert(out.end(), 'y'); out.insert(out.end(), 'p');
        append(out, payload);
        return out;
    }

    uint32_t minorVersion = rd32(&srcFtypBox[12]);

    std::vector<std::string> compatBrands;
    size_t pos = 16;
    while (pos + 4 <= srcFtypBox.size()) {
        std::string brand = fourccStr(&srcFtypBox[pos]);
        if (brand != "dash") {
            compatBrands.push_back(brand);
        }
        pos += 4;
    }
    // Make sure "isom" itself is present among compatible brands.
    bool hasIsom = false;
    for (size_t i = 0; i < compatBrands.size(); ++i) {
        if (compatBrands[i] == "isom") { hasIsom = true; break; }
    }
    if (!hasIsom) {
        compatBrands.insert(compatBrands.begin(), "isom");
    }

    Mp4RemuxBytes payload;
    // major_brand = "isom"
    payload.insert(payload.end(), 'i'); payload.insert(payload.end(), 's');
    payload.insert(payload.end(), 'o'); payload.insert(payload.end(), 'm');
    wr32(payload, minorVersion);
    for (size_t i = 0; i < compatBrands.size(); ++i) {
        const std::string &b = compatBrands[i];
        for (size_t c = 0; c < 4; ++c) {
            payload.push_back(c < b.size() ? uint8_t(b[c]) : uint8_t(' '));
        }
    }

    Mp4RemuxBytes out;
    wr32(out, uint32_t(8 + payload.size()));
    out.insert(out.end(), 'f'); out.insert(out.end(), 't');
    out.insert(out.end(), 'y'); out.insert(out.end(), 'p');
    append(out, payload);
    return out;
}

static BoxLoc findBoxOrThrow(const Mp4RemuxBytes &data, size_t start, size_t end,
                              const std::string &sourceLabel, const char *boxName,
                              bool allowPayloadBeyondEnd) {
    BoxLoc loc = findBox(data, start, end, boxName, allowPayloadBeyondEnd);
    if (!loc.found)
        throw std::runtime_error(sourceLabel + ": no " + std::string(boxName) +
                                  " box found in head buffer (fetch a larger head?)");
    return loc;
}

// ---------------------------------------------------------------------------
// parseHead (public)
// ---------------------------------------------------------------------------
TrackHead parseHead(const Mp4RemuxBytes &headBytes, const std::string &label) {
    TrackHead t;
    t.label = label;
    const Mp4RemuxBytes &d = headBytes;
    size_t bufEnd = d.size();

    BoxLoc ftyp = findBoxOrThrow(d, 0, bufEnd, label, "ftyp");
    t.ftypBox = rebuildFtypBox(sliceBox(d, ftyp));

    BoxLoc moov = findBoxOrThrow(d, 0, bufEnd, label, "moov");
    BoxLoc mdat = findBoxOrThrow(d, 0, bufEnd, label, "mdat", /*allowPayloadBeyondEnd=*/true);
    t.mdatBodyOffsetInSource = mdat.payloadOffset;
    t.mdatDeclaredSize = mdat.payloadSize;

    BoxLoc trak = findBoxOrThrow(d, moov.payloadOffset, moov.payloadOffset + moov.payloadSize, label, "trak");
    size_t trakStart = trak.payloadOffset, trakEnd = trak.payloadOffset + trak.payloadSize;

    BoxLoc tkhd = findBoxOrThrow(d, trakStart, trakEnd, label, "tkhd");
    t.tkhdBox = sliceBox(d, tkhd);

    BoxLoc mdia = findBoxOrThrow(d, trakStart, trakEnd, label, "mdia");
    size_t mdiaStart = mdia.payloadOffset, mdiaEnd = mdia.payloadOffset + mdia.payloadSize;

    BoxLoc mdhd = findBoxOrThrow(d, mdiaStart, mdiaEnd, label, "mdhd");
    t.mdhdBox = sliceBox(d, mdhd);
    {
        const uint8_t *p = &d[mdhd.payloadOffset];
        uint8_t version = p[0];
        if (version == 1) {
            t.timescale = rd32(&d[mdhd.payloadOffset + 20]);
            t.duration = rd64(&d[mdhd.payloadOffset + 24]);
        } else {
            t.timescale = rd32(&d[mdhd.payloadOffset + 12]);
            t.duration = rd32(&d[mdhd.payloadOffset + 16]);
        }
    }

    BoxLoc hdlr = findBoxOrThrow(d, mdiaStart, mdiaEnd, label, "hdlr");
    t.hdlrBox = sliceBox(d, hdlr);
    t.isVideo = (fourccStr(&d[hdlr.payloadOffset + 8]) == "vide");

    BoxLoc minf = findBoxOrThrow(d, mdiaStart, mdiaEnd, label, "minf");
    size_t minfStart = minf.payloadOffset, minfEnd = minf.payloadOffset + minf.payloadSize;

    BoxLoc mhd = t.isVideo ? findBoxOrThrow(d, minfStart, minfEnd, label, "vmhd")
                            : findBoxOrThrow(d, minfStart, minfEnd, label, "smhd");
    t.mediaHeaderBox = sliceBox(d, mhd);

    BoxLoc stbl = findBoxOrThrow(d, minfStart, minfEnd, label, "stbl");
    size_t stblStart = stbl.payloadOffset, stblEnd = stbl.payloadOffset + stbl.payloadSize;

    t.stsdBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "stsd"));
    t.sttsBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "stts"));
    t.stscBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "stsc"));
    t.stszBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "stsz"));

    BoxLoc ctts = findBox(d, stblStart, stblEnd, "ctts");
    if (ctts.found) t.cttsBox = sliceBox(d, ctts);
    if (t.isVideo) {
        BoxLoc stss = findBox(d, stblStart, stblEnd, "stss");
        if (stss.found) t.stssBox = sliceBox(d, stss);
    }

    BoxLoc stco = findBox(d, stblStart, stblEnd, "stco");
    BoxLoc co64 = stco.found ? BoxLoc() : findBox(d, stblStart, stblEnd, "co64");
    uint32_t stcoEntryCount = 0;
    if (stco.found && stco.payloadSize >= 8) {
        stcoEntryCount = rd32(&d[stco.payloadOffset + 4]);
    } else if (co64.found && co64.payloadSize >= 8) {
        stcoEntryCount = rd32(&d[co64.payloadOffset + 4]);
    }

    if (stcoEntryCount == 0) {
        // Empty stco/co64 (0 entries) means this is a DASH-style
        // fragmented mp4 (YouTube/Invidious adaptiveFormats): moov only
        // carries mvex/trex defaults, and the real per-sample offsets live
        // in each fragment's moof/traf/trun -- see mp4_stream_remux.hpp for
        // the caller workflow. Leave stts/stsz/stco unpopulated; the
        // caller must collect fragSamples via parseMoofSamples() and then
        // call buildProgressiveTablesFromFragments().
        t.isFragmented = true;
        // mdat found above (via allowPayloadBeyondEnd) is actually the
        // FIRST fragment's mdat in this case, not a single track-wide
        // mdat -- mdatBodyOffsetInSource/mdatDeclaredSize will be
        // recomputed by buildProgressiveTablesFromFragments() once all
        // fragments are collected, so clear them here to avoid confusion
        // if a caller reads them before that.
        t.mdatBodyOffsetInSource = 0;
        t.mdatDeclaredSize = 0;
        // sidx normally sits immediately after moov (before the first
        // moof). Search the whole head buffer for it; if the head fetch
        // wasn't large enough to reach it, sidx.found will be false and
        // the caller must refetch with a larger head size.
        size_t moovEnd = moov.headerOffset + moov.totalSize;
        BoxLoc sidxLoc = findBox(d, moovEnd, bufEnd, "sidx");
        if (sidxLoc.found) {
            t.sidx = parseSidx(d, moovEnd, bufEnd, sidxLoc.headerOffset);
        }
        return t;
    }

    if (stco.found) {
        t.stcoIs64 = false;
        t.stcoBox = sliceBox(d, stco);
    } else {
        t.stcoIs64 = true;
        t.stcoBox = sliceBox(d, findBoxOrThrow(d, stblStart, stblEnd, label, "co64"));
    }

    return t;
}

// ---------------------------------------------------------------------------
// moov construction helpers (internal)
// ---------------------------------------------------------------------------
static void shiftChunkOffsets(Mp4RemuxBytes &box, bool is64, int64_t delta) {
    uint32_t entryCount = rd32(&box[12]);
    size_t entrySize = is64 ? 8 : 4;
    size_t entriesStart = 16;
    for (uint32_t i = 0; i < entryCount; i++) {
        size_t off = entriesStart + size_t(i) * entrySize;
        if (is64) {
            uint64_t v = rd64(&box[off]);
            v = uint64_t(int64_t(v) + delta);
            box[off+0]=uint8_t(v>>56); box[off+1]=uint8_t(v>>48); box[off+2]=uint8_t(v>>40); box[off+3]=uint8_t(v>>32);
            box[off+4]=uint8_t(v>>24); box[off+5]=uint8_t(v>>16); box[off+6]=uint8_t(v>>8);  box[off+7]=uint8_t(v);
        } else {
            uint32_t v = rd32(&box[off]);
            v = uint32_t(int64_t(v) + delta);
            box[off+0]=uint8_t(v>>24); box[off+1]=uint8_t(v>>16); box[off+2]=uint8_t(v>>8); box[off+3]=uint8_t(v);
        }
    }
}
// shiftChunkOffsets() above assumes every sample's gap to the next one in
// the OUTPUT file is identical to its gap in the SOURCE (it just adds one
// constant delta to every stco entry), which only holds when the body is
// written to the output exactly as laid out in the source. That's true for
// a plain (non-fragmented) track, whose mdat is one contiguous span both
// before and after remuxing.
//
// It's false for a fragmented DASH source: consecutive fragments' mdat
// payloads are usually back-to-back in the source, but NOT always -- e.g.
// the byte range between one fetched batch's samples and the next can have
// a gap (moof/other box bytes, or simply a batch boundary that didn't land
// on a contiguous run -- see buildVideoBodyBatches()'s "contiguous" check).
// The body-download path (onVideoBodyBatchFinished()/onAudioFragBodyFinished())
// writes every sample to the output back-to-back regardless -- no gaps in
// the output file, ever -- but shiftChunkOffsets() doesn't know that: it
// preserves whatever gaps existed in the source, so every sample after the
// first such gap ends up with a wrong (too-large) declared offset in stco,
// pointing past where its bytes actually are. This was found in the field
// via ffprobe/ffmpeg analysis of an actual remuxed file: the first 136
// video samples (before the first inter-batch gap) decoded fine, and every
// sample after that was corrupt -- matching "plays for ~5 seconds then
// fails" exactly.
//
// Fix: for a fragmented track, rebuild stco from scratch using each
// sample's actual (gapless) position in the output -- a running prefix sum
// of fragSamples[i].size, the same math the body-download path itself uses
// to compute where each sample's bytes really land.
static void rebuildStcoContiguous(TrackHead &track, uint64_t outputMdatStart) {
    uint32_t entryCount = uint32_t(track.fragSamples.size());
    size_t entrySize = track.stcoIs64 ? 8 : 4;
    size_t entriesStart = 16;
    if (track.stcoBox.size() < entriesStart + size_t(entryCount) * entrySize) {
        throw std::runtime_error(track.label + ": stco box too small to hold "
                + track.label + "'s fragSamples entries");
    }

    uint64_t cursor = outputMdatStart;
    for (uint32_t i = 0; i < entryCount; i++) {
        size_t off = entriesStart + size_t(i) * entrySize;
        if (track.stcoIs64) {
            uint64_t v = cursor;
            track.stcoBox[off+0]=uint8_t(v>>56); track.stcoBox[off+1]=uint8_t(v>>48);
            track.stcoBox[off+2]=uint8_t(v>>40); track.stcoBox[off+3]=uint8_t(v>>32);
            track.stcoBox[off+4]=uint8_t(v>>24); track.stcoBox[off+5]=uint8_t(v>>16);
            track.stcoBox[off+6]=uint8_t(v>>8);  track.stcoBox[off+7]=uint8_t(v);
        } else {
            uint32_t v = uint32_t(cursor);
            track.stcoBox[off+0]=uint8_t(v>>24); track.stcoBox[off+1]=uint8_t(v>>16);
            track.stcoBox[off+2]=uint8_t(v>>8);  track.stcoBox[off+3]=uint8_t(v);
        }
        cursor += track.fragSamples[i].size;
    }
}
static void patchTkhdTrackId(Mp4RemuxBytes &tkhd, uint32_t newId) {
    uint8_t version = tkhd[8];
    size_t idOff = 8 + ((version == 1) ? 20 : 12);
    tkhd[idOff+0]=uint8_t(newId>>24); tkhd[idOff+1]=uint8_t(newId>>16);
    tkhd[idOff+2]=uint8_t(newId>>8);  tkhd[idOff+3]=uint8_t(newId);
}
static Mp4RemuxBytes makeBox(const std::string &type, const Mp4RemuxBytes &payload) {
    Mp4RemuxBytes out;
    wr32(out, uint32_t(payload.size() + 8));
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
static Mp4RemuxBytes buildTrak(TrackHead &src, uint32_t newTrackId) {
    patchTkhdTrackId(src.tkhdBox, newTrackId);
    Mp4RemuxBytes stbl;
    append(stbl, src.stsdBox);
    append(stbl, src.sttsBox);
    if (!src.cttsBox.empty()) append(stbl, src.cttsBox);
    if (!src.stssBox.empty()) append(stbl, src.stssBox);
    append(stbl, src.stscBox);
    append(stbl, src.stszBox);
    append(stbl, src.stcoBox);
    Mp4RemuxBytes stblBox = makeBox("stbl", stbl);

    Mp4RemuxBytes urlEntry;
    wr32(urlEntry, 12);
    urlEntry.push_back('u'); urlEntry.push_back('r'); urlEntry.push_back('l'); urlEntry.push_back(' ');
    wr32(urlEntry, 0x00000001);
    Mp4RemuxBytes drefPayload; wr32(drefPayload, 0); wr32(drefPayload, 1); append(drefPayload, urlEntry);
    Mp4RemuxBytes dinf = makeBox("dinf", makeBox("dref", drefPayload));

    Mp4RemuxBytes minfPayload;
    append(minfPayload, src.mediaHeaderBox);
    append(minfPayload, dinf);
    append(minfPayload, stblBox);
    Mp4RemuxBytes minfBox = makeBox("minf", minfPayload);

    Mp4RemuxBytes mdiaPayload;
    append(mdiaPayload, src.mdhdBox);
    append(mdiaPayload, src.hdlrBox);
    append(mdiaPayload, minfBox);
    Mp4RemuxBytes mdiaBox = makeBox("mdia", mdiaPayload);

    Mp4RemuxBytes trakPayload;
    append(trakPayload, src.tkhdBox);
    append(trakPayload, mdiaBox);
    return makeBox("trak", trakPayload);
}
static Mp4RemuxBytes buildMvhd(uint32_t timescale, uint64_t duration, uint32_t nextTrackId) {
    Mp4RemuxBytes p;
    p.push_back(0); p.push_back(0); p.push_back(0); p.push_back(0);
    wr32(p, 0); wr32(p, 0);
    wr32(p, timescale);
    wr32(p, uint32_t(duration));
    wr32(p, 0x00010000);
    p.push_back(0x01); p.push_back(0x00); p.push_back(0); p.push_back(0);
    wr32(p, 0); wr32(p, 0);
    static const uint32_t matrix[9] = {0x00010000,0,0, 0,0x00010000,0, 0,0,0x40000000};
    for (int i = 0; i < 9; i++) wr32(p, matrix[i]);
    for (int i = 0; i < 6; i++) wr32(p, 0);
    wr32(p, nextTrackId);
    return makeBox("mvhd", p);
}
static Mp4RemuxBytes buildFtypMoov(TrackHead &v, TrackHead &a) {
    uint32_t movieTimescale = 1000;
    uint64_t vDurMs = uint64_t(double(v.duration) / v.timescale * 1000.0);
    uint64_t aDurMs = uint64_t(double(a.duration) / a.timescale * 1000.0);
    uint64_t movieDur = std::max(vDurMs, aDurMs);

    Mp4RemuxBytes mvhd = buildMvhd(movieTimescale, movieDur, 3);
    Mp4RemuxBytes trakV = buildTrak(v, 1);
    Mp4RemuxBytes trakA = buildTrak(a, 2);

    Mp4RemuxBytes moovPayload;
    append(moovPayload, mvhd);
    append(moovPayload, trakV);
    append(moovPayload, trakA);
    Mp4RemuxBytes moovBox = makeBox("moov", moovPayload);

    Mp4RemuxBytes out;
    append(out, v.ftypBox);
    append(out, moovBox);
    return out;
}

// ---------------------------------------------------------------------------
// planStreamingRemux (public)
// ---------------------------------------------------------------------------
bool planStreamingRemux(TrackHead &videoHead, TrackHead &audioHead,
                         RemuxPlan *outPlan, std::string *errorOut) {
    try {
        if (!videoHead.isVideo) throw std::runtime_error("videoHead is not a video track");
        if (audioHead.isVideo)  throw std::runtime_error("audioHead is not an audio track");

        Mp4RemuxBytes head = buildFtypMoov(videoHead, audioHead);
        size_t mdatHeaderSize = 8;
        size_t outMdatStart = head.size() + mdatHeaderSize;

        // Audio goes first (small, finishes fast -- gets the whole
        // duration's audio available almost immediately); video streams
        // in after it.
        size_t audioOutStart = outMdatStart;
        size_t videoOutStart = outMdatStart + size_t(audioHead.mdatDeclaredSize);

        int64_t audioDelta = int64_t(audioOutStart) - int64_t(audioHead.mdatBodyOffsetInSource);
        int64_t videoDelta = int64_t(videoOutStart) - int64_t(videoHead.mdatBodyOffsetInSource);
        fprintf(stderr, "[bbtube][remux][debug] planStreamingRemux "
                "audioOutStart=%zu audioHead.mdatBodyOffsetInSource=%zu audioDelta=%lld "
                "videoOutStart=%zu videoHead.mdatBodyOffsetInSource=%zu videoDelta=%lld\n",
                audioOutStart, audioHead.mdatBodyOffsetInSource, (long long)audioDelta,
                videoOutStart, videoHead.mdatBodyOffsetInSource, (long long)videoDelta);
        // Fragmented (DASH) tracks: samples can have gaps between them in
        // the source that the body-download path doesn't preserve in the
        // output (see rebuildStcoContiguous()'s comment), so stco must be
        // rebuilt from each sample's actual gapless output position rather
        // than shifted by a single constant delta. Non-fragmented tracks'
        // mdat is one contiguous span in both source and output, so the
        // constant-delta shift remains correct (and cheaper) for them.
        if (audioHead.isFragmented) {
            rebuildStcoContiguous(audioHead, uint64_t(audioOutStart));
        } else {
            shiftChunkOffsets(audioHead.stcoBox, audioHead.stcoIs64, audioDelta);
        }
        if (videoHead.isFragmented) {
            rebuildStcoContiguous(videoHead, uint64_t(videoOutStart));
        } else {
            shiftChunkOffsets(videoHead.stcoBox, videoHead.stcoIs64, videoDelta);
        }
        {
            uint32_t vEntryCount = videoHead.stcoBox.size() >= 16 ? rd32(&videoHead.stcoBox[12]) : 0;
            uint32_t aEntryCount = audioHead.stcoBox.size() >= 16 ? rd32(&audioHead.stcoBox[12]) : 0;
            size_t vEntrySize = videoHead.stcoIs64 ? 8 : 4;
            size_t aEntrySize = audioHead.stcoIs64 ? 8 : 4;
            fprintf(stderr, "[bbtube][remux][debug] post-shift stco video[0]=%llu audio[0]=%llu\n",
                    vEntryCount > 0 ? (unsigned long long)(videoHead.stcoIs64 ? rd64(&videoHead.stcoBox[16]) : rd32(&videoHead.stcoBox[16])) : 0ULL,
                    aEntryCount > 0 ? (unsigned long long)(audioHead.stcoIs64 ? rd64(&audioHead.stcoBox[16]) : rd32(&audioHead.stcoBox[16])) : 0ULL);
            (void)vEntrySize; (void)aEntrySize;
        }

        Mp4RemuxBytes finalHead = buildFtypMoov(videoHead, audioHead);
        if (finalHead.size() != head.size())
            throw std::runtime_error("internal error: moov size changed between passes");

        uint64_t totalMdat = mdatHeaderSize + audioHead.mdatDeclaredSize + videoHead.mdatDeclaredSize;
        Mp4RemuxBytes mdatHeader;
        wr32(mdatHeader, uint32_t(totalMdat));
        mdatHeader.push_back('m'); mdatHeader.push_back('d'); mdatHeader.push_back('a'); mdatHeader.push_back('t');

        outPlan->headBytes = finalHead;
        outPlan->headBytes.insert(outPlan->headBytes.end(), mdatHeader.begin(), mdatHeader.end());
        outPlan->audioOutputOffset = audioOutStart;
        outPlan->audioBodySize = audioHead.mdatDeclaredSize;
        outPlan->videoOutputOffset = videoOutStart;
        outPlan->videoBodySize = videoHead.mdatDeclaredSize;
        outPlan->totalOutputSize = outMdatStart + audioHead.mdatDeclaredSize + videoHead.mdatDeclaredSize;
        return true;
    } catch (const std::exception &e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// I/O helpers (public). Deliberately plain fstream (not Qt) so this file
// has no Qt dependency and stays independently testable; StreamingRemuxSession
// wraps these with QNetworkReply-driven calls.
// ---------------------------------------------------------------------------
bool preallocateAndWriteHead(const std::string &outputPath, const RemuxPlan &plan, std::string *errorOut) {
    try {
        std::ofstream out(outputPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot create output: " + outputPath);
        if (plan.totalOutputSize > 0) {
            out.seekp(std::streamoff(plan.totalOutputSize - 1));
            char zero = 0;
            out.write(&zero, 1);
        }
        out.seekp(0);
        out.write(reinterpret_cast<const char *>(&plan.headBytes[0]), std::streamsize(plan.headBytes.size()));
        return true;
    } catch (const std::exception &e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }
}

bool writeBodyChunk(const std::string &outputPath, uint64_t outputOffset,
                     const uint8_t *data, size_t len, std::string *errorOut) {
    try {
        std::fstream out(outputPath.c_str(), std::ios::binary | std::ios::in | std::ios::out);
        if (!out) throw std::runtime_error("cannot open output for writing: " + outputPath);
        out.seekp(std::streamoff(outputOffset));
        out.write(reinterpret_cast<const char *>(data), std::streamsize(len));
        return bool(out);
    } catch (const std::exception &e) {
        if (errorOut) *errorOut = e.what();
        return false;
    }
}
