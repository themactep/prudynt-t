#ifndef IMP_BACKCHANNEL_HPP
#define IMP_BACKCHANNEL_HPP

/*
 *  Backchannel Audio Pipeline: Architecture Overview
 *
 *  This module manages backchannel audio, enabling two-way audio
 *  communication from an RTSP client. It sets up IMP audio decoder
 *  channels and ties the RTSP backchannel sink to the main
 *  AudioOutputWorker so decoded speech can be played on the camera.
 *
 *  Pipeline overview:
 *   1. RTSP Setup: The RTSP server adds a BackchannelServerMediaSubsession
 *      for each supported codec. The SDP advertises payload type, sample
 *      rate, and encoding name to the client.
 *   2. Ingest: BackchannelSink receives RTP payloads from the client,
 *      enforces an inactivity timeout, and emits BackchannelFrame objects
 *      into global_backchannel->inputQueue. Zero-length frames act as
 *      stop signals; normal frames carry encoded audio and the RTSP
 *      session id.
 *   3. Processing: BackchannelWorker waits for frames, keeps track of the
 *      currently active session, and decodes payloads via IMP decoders
 *      (AAC/PCMU/PCMA, etc). Optional resampling brings the PCM to the
 *      configured output sample rate.
 *   4. Playback: The worker forwards decoded PCM to
 *      AudioOutputWorker::enqueuePcm(), which feeds the same IMP audio
 *      output path used by local sound effects.
 *
 *  The IMPBackchannel class is responsible for:
 *   - Registering IMP decoder channels for the supported backchannel
 *     codecs.
 *   - Creating and destroying the IMP audio resources used by the worker.
 */

#include "Config.hpp"

// Define the list of backchannel formats and their properties
// X(EnumName, NameString, PayloadType, Frequency, MimeType)
// https://www.rfc-editor.org/rfc/rfc3640.html
#if defined(USE_AAC) && USE_AAC
#define X_FOREACH_BACKCHANNEL_FORMAT(X)                                        \
  X(AAC, "mpeg4-generic", 97, cfg->audio.output_sample_rate,                   \
    "audio/mpeg4-generic")                                                     \
  X(PCMU, "PCMU", 0, 8000, "audio/PCMU")                                       \
  X(PCMA, "PCMA", 8, 8000, "audio/PCMA")                                       \
  /* Add new formats here */
#else
#define X_FOREACH_BACKCHANNEL_FORMAT(X)                                        \
  X(PCMU, "PCMU", 0, 8000, "audio/PCMU")                                       \
  X(PCMA, "PCMA", 8, 8000, "audio/PCMA")                                       \
  /* Add new formats here */
#endif

#define APPLY_ENUM(EnumName, NameString, PayloadType, Frequency, MimeType)     \
  EnumName,
enum class IMPBackchannelFormat {
  UNKNOWN = -1,
  X_FOREACH_BACKCHANNEL_FORMAT(APPLY_ENUM)
};
#undef APPLY_ENUM

class IMPBackchannel {
public:
  static IMPBackchannel *createNew();
  IMPBackchannel() {
    init();
  }
  ~IMPBackchannel() {
    deinit();
  };
  int init();
  void deinit();
  int ensureDecoderChannel(IMPBackchannelFormat format);

  static const char *getFormatName(IMPBackchannelFormat format) {
#define RETURN_NAME(EnumName, NameString, PayloadType, Frequency, MimeType)    \
  {                                                                            \
    if (IMPBackchannelFormat::EnumName == format)                              \
      return NameString;                                                       \
  }
    X_FOREACH_BACKCHANNEL_FORMAT(RETURN_NAME)
#undef RETURN_NAME
    return "UNKNOWN";
  }

  static int getFormatPayloadType(IMPBackchannelFormat format) {
#define RETURN_PAYLOADTYPE(EnumName, NameString, PayloadType, Frequency,       \
                           MimeType)                                           \
  {                                                                            \
    if (IMPBackchannelFormat::EnumName == format)                              \
      return PayloadType;                                                      \
  }
    X_FOREACH_BACKCHANNEL_FORMAT(RETURN_PAYLOADTYPE)
#undef RETURN_PAYLOADTYPE
    return 96;
  }

  static int getFormatFrequency(IMPBackchannelFormat format) {
#define RETURN_FREQUENCY(EnumName, NameString, PayloadType, Frequency,         \
                         MimeType)                                             \
  {                                                                            \
    if (IMPBackchannelFormat::EnumName == format)                              \
      return Frequency;                                                        \
  }
    X_FOREACH_BACKCHANNEL_FORMAT(RETURN_FREQUENCY)
#undef RETURN_FREQUENCY
    return 0;
  }

  static const char *getFormatMimeType(IMPBackchannelFormat format) {
#define RETURN_MIME_TYPE(EnumName, NameString, PayloadType, Frequency,         \
                         MimeType)                                             \
  {                                                                            \
    if (IMPBackchannelFormat::EnumName == format)                              \
      return MimeType;                                                         \
  }
    X_FOREACH_BACKCHANNEL_FORMAT(RETURN_MIME_TYPE)
#undef RETURN_MIME_TYPE
    return "audio/unknown";
  }

private:
  int aacDecoderHandle{-1};
};

#endif // IMP_BACKCHANNEL_HPP
