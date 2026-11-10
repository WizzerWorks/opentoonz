#include "tscannersane.h"
#include "texception.h"
#include "trasterimage.h"
#include "traster.h"

#include <QCoreApplication>
#include <QSettings>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <sane/sane.h>

namespace {

const char *SettingsKey = "CurrentSaneDevice";

bool saneInitialized = false;
bool deviceSelected  = false;

// sane_init() must be called once per process, sane_exit() at the end.
bool ensureInit() {
  if (saneInitialized) return true;
  SANE_Int version = 0;
  if (sane_init(&version, nullptr) != SANE_STATUS_GOOD) return false;
  saneInitialized = true;
  return true;
}

struct DeviceInfo {
  QString name;    // SANE identifier, e.g. "genesys:libusb:001:004"
  QString vendor;  // e.g. "Canon"
  QString model;   // e.g. "LiDE 220"
};

QList<DeviceInfo> queryDevices() {
  QList<DeviceInfo> result;
  if (!ensureInit()) return result;
  const SANE_Device **devs = nullptr;
  // local_only = false: also list network (saned) devices
  if (sane_get_devices(&devs, SANE_FALSE) != SANE_STATUS_GOOD || !devs)
    return result;
  for (int i = 0; devs[i]; ++i) {
    DeviceInfo d;
    d.name   = QString::fromUtf8(devs[i]->name);
    d.vendor = QString::fromUtf8(devs[i]->vendor);
    d.model  = QString::fromUtf8(devs[i]->model);
    result.append(d);
  }
  return result;
}


//-----------------------------------------------------------------------------
// Option probing
//-----------------------------------------------------------------------------

// A numeric option: its index on the device, and the range it accepts.
struct NumOpt {
  int index = -1;  // -1: option not present / not usable
  SANE_Value_Type type = SANE_TYPE_INT;
  SANE_Unit unit       = SANE_UNIT_NONE;
  double min = 0, max = 0, step = 0;
  bool valid() const { return index > 0; }
};

struct Caps {
  NumOpt resolution, tlx, tly, brx, bry, brightness, threshold;
  int sourceIndex = -1;
  QString srcFlatbed, srcAdf;  // device spelling of the paper sources
  int modeIndex = -1;
  QString modeBW, modeGray, modeColor;  // device spelling of each mode
};

double toDouble(SANE_Value_Type t, SANE_Word w) {
  return t == SANE_TYPE_FIXED ? SANE_UNFIX(w) : (double)w;
}

SANE_Word fromDouble(SANE_Value_Type t, double v) {
  return t == SANE_TYPE_FIXED ? SANE_FIX(v) : (SANE_Word)std::lround(v);
}

NumOpt readNumOpt(const SANE_Option_Descriptor *d, int index) {
  NumOpt o;
  if (!d || !SANE_OPTION_IS_ACTIVE(d->cap) || !SANE_OPTION_IS_SETTABLE(d->cap))
    return o;
  if (d->type != SANE_TYPE_INT && d->type != SANE_TYPE_FIXED) return o;
  o.type = d->type;
  o.unit = d->unit;
  if (d->constraint_type == SANE_CONSTRAINT_RANGE && d->constraint.range) {
    o.min  = toDouble(d->type, d->constraint.range->min);
    o.max  = toDouble(d->type, d->constraint.range->max);
    o.step = toDouble(d->type, d->constraint.range->quant);
  } else if (d->constraint_type == SANE_CONSTRAINT_WORD_LIST &&
             d->constraint.word_list && d->constraint.word_list[0] > 0) {
    const SANE_Word *w = d->constraint.word_list;
    o.min = o.max = toDouble(d->type, w[1]);
    for (int i = 2; i <= w[0]; ++i) {
      o.min = std::min(o.min, toDouble(d->type, w[i]));
      o.max = std::max(o.max, toDouble(d->type, w[i]));
    }
  } else
    return o;  // unconstrained: we cannot report a range
  o.index = index;
  return o;
}

// Backends spell the mode names differently ("Lineart", "Binary", "Gray",
// "Grayscale", "Color", "Colour", "True Gray"...).
void classifyMode(const QString &m, Caps &c) {
  QString l = m.toLower();
  QString *slot = nullptr;
  if (l.contains("gray") || l.contains("grey"))
    slot = &c.modeGray;
  else if (l.contains("color") || l.contains("colour") || l == "rgb")
    slot = &c.modeColor;
  else if (l.contains("lineart") || l.contains("binary") ||
           l.contains("black"))
    slot = &c.modeBW;
  if (slot && slot->isEmpty()) *slot = m;
}

void classifySource(const QString &m, Caps &c) {
  QString l = m.toLower();
  if (l.contains("flatbed")) {
    if (c.srcFlatbed.isEmpty()) c.srcFlatbed = m;
  } else if (l.contains("adf") || l.contains("feeder")) {
    // prefer a simplex source over "ADF Duplex" / "ADF Back"
    bool simplex = !l.contains("duplex") && !l.contains("back");
    if (c.srcAdf.isEmpty() || (simplex && c.srcAdf.toLower().contains("duplex")))
      c.srcAdf = m;
  }
}

// Human readable explanation of a SANE status
std::string describe(SANE_Status st) {
  switch (st) {
  case SANE_STATUS_ACCESS_DENIED:
    return "access to the scanner was denied (check that your user is allowed "
           "to use scanners, e.g. membership of the 'scanner' group)";
  case SANE_STATUS_DEVICE_BUSY:
    return "the scanner is busy";
  case SANE_STATUS_COVER_OPEN:
    return "the scanner cover is open";
  case SANE_STATUS_JAMMED:
    return "paper jam in the feeder";
  case SANE_STATUS_NO_DOCS:
    return "no paper in the feeder";
  case SANE_STATUS_IO_ERROR:
    return "communication error with the scanner";
  default:
    return sane_strstatus(st);
  }
}

Caps probe(SANE_Handle h) {
  Caps c;
  const SANE_Option_Descriptor *d0 = sane_get_option_descriptor(h, 0);
  if (!d0) return c;
  SANE_Int count = 0;
  if (sane_control_option(h, 0, SANE_ACTION_GET_VALUE, &count, nullptr) !=
      SANE_STATUS_GOOD)
    return c;
  for (int i = 1; i < count; ++i) {
    const SANE_Option_Descriptor *d = sane_get_option_descriptor(h, i);
    if (!d || !d->name) continue;
    QString n = QString::fromUtf8(d->name);
    if (n == "resolution")
      c.resolution = readNumOpt(d, i);
    else if (n == "tl-x")
      c.tlx = readNumOpt(d, i);
    else if (n == "tl-y")
      c.tly = readNumOpt(d, i);
    else if (n == "br-x")
      c.brx = readNumOpt(d, i);
    else if (n == "br-y")
      c.bry = readNumOpt(d, i);
    else if (n == "brightness")
      c.brightness = readNumOpt(d, i);
    else if (n == "threshold")
      c.threshold = readNumOpt(d, i);
    else if (n == "source" && d->type == SANE_TYPE_STRING &&
             d->constraint_type == SANE_CONSTRAINT_STRING_LIST &&
             SANE_OPTION_IS_SETTABLE(d->cap)) {
      c.sourceIndex = i;
      for (const SANE_String_Const *s = d->constraint.string_list; s && *s; ++s)
        classifySource(QString::fromUtf8(*s), c);
    } else if (n == "mode" && d->type == SANE_TYPE_STRING &&
             d->constraint_type == SANE_CONSTRAINT_STRING_LIST &&
             SANE_OPTION_IS_SETTABLE(d->cap)) {
      c.modeIndex = i;
      for (const SANE_String_Const *s = d->constraint.string_list; s && *s; ++s)
        classifyMode(QString::fromUtf8(*s), c);
    }
  }
  return c;
}

// RAII wrapper around an open device
class Device {
public:
  explicit Device(const QString &name) {
    SANE_Status st = sane_open(name.toUtf8().constData(), &m_h);
    if (st != SANE_STATUS_GOOD)
      throw TException(std::string("Cannot open SANE device: ") +
                       describe(st));
  }
  ~Device() {
    if (m_h) {
      sane_cancel(m_h);
      sane_close(m_h);
    }
  }
  SANE_Handle handle() const { return m_h; }

private:
  SANE_Handle m_h = nullptr;
  Device(const Device &)            = delete;
  Device &operator=(const Device &) = delete;
};

// The device the user selected (or the first one found). Throws if none.
QString deviceToOpen() {
  QString name = QSettings().value(SettingsKey).toString();
  if (!name.isEmpty()) return name;
  QList<DeviceInfo> devs = queryDevices();
  if (devs.isEmpty()) throw TException("No SANE scanner is available");
  return devs.first().name;
}

// Sets a value; re-probes the option set if the backend says it changed.
void setNumeric(SANE_Handle h, Caps &caps, const NumOpt &o, double value) {
  if (!o.valid()) return;
  value = std::min(std::max(value, o.min), o.max);
  SANE_Word w   = fromDouble(o.type, value);
  SANE_Int info = 0;
  SANE_Status st =
      sane_control_option(h, o.index, SANE_ACTION_SET_VALUE, &w, &info);
  if (st != SANE_STATUS_GOOD)
    throw TException(std::string("Cannot set SANE option: ") +
                     describe(st));
  if (info & SANE_INFO_RELOAD_OPTIONS) caps = probe(h);
}

void setStringOption(SANE_Handle h, Caps &caps, int index,
                     const QString &mode) {
  QByteArray b = mode.toUtf8();
  std::vector<char> buf(std::max<int>(b.size() + 1, 64), 0);
  memcpy(buf.data(), b.constData(), b.size());
  SANE_Int info = 0;
  SANE_Status st =
      sane_control_option(h, index, SANE_ACTION_SET_VALUE, buf.data(), &info);
  if (st != SANE_STATUS_GOOD)
    throw TException(std::string("Cannot set a SANE option: ") +
                     describe(st));
  if (info & SANE_INFO_RELOAD_OPTIONS) caps = probe(h);
}

// OpenToonz brightness/threshold use 0..255; map that onto the device range
double fromUiRange(const NumOpt &o, double v) {
  return o.min + std::min(std::max(v, 0.0), 255.0) / 255.0 * (o.max - o.min);
}

double getNumeric(SANE_Handle h, const NumOpt &o, double fallback) {
  SANE_Word w = 0;
  if (!o.valid() ||
      sane_control_option(h, o.index, SANE_ACTION_GET_VALUE, &w, nullptr) !=
          SANE_STATUS_GOOD)
    return fallback;
  return toDouble(o.type, w);
}

// Geometry options are in mm, except on some backends which use pixels at the
// current resolution.
double mmToDevice(const NumOpt &o, double mm, double dpi) {
  return o.unit == SANE_UNIT_PIXEL ? mm / 25.4 * dpi : mm;
}
double deviceToMm(const NumOpt &o, double v, double dpi) {
  return o.unit == SANE_UNIT_PIXEL ? v / dpi * 25.4 : v;
}

//-----------------------------------------------------------------------------
// Image conversion
//-----------------------------------------------------------------------------

struct Image {
  SANE_Parameters p;
  std::vector<unsigned char> data;  // top-down rows, bytes_per_line each
  int lines = 0;                    // lines actually received
};

// SANE hands out rows top to bottom; TRaster row 0 is the bottom one.
TRasterP convert(const Image &img, bool softwareThreshold, float threshold) {
  const SANE_Parameters &p = img.p;
  const int lx = p.pixels_per_line, ly = img.lines;
  const int depth = p.depth;
  if (lx <= 0 || ly <= 0) return TRasterP();

  auto row = [&](int y) { return img.data.data() + (size_t)y * p.bytes_per_line; };

  if (p.format == SANE_FRAME_RGB && (depth == 8 || depth == 16)) {
    TRaster32P ras(lx, ly);
    for (int y = 0; y < ly; ++y) {
      const unsigned char *src = row(y);
      TPixel32 *dst            = ras->pixels(ly - 1 - y);
      for (int x = 0; x < lx; ++x, ++dst) {
        unsigned char c[3];
        for (int k = 0; k < 3; ++k) {
          if (depth == 8)
            c[k] = src[x * 3 + k];
          else {  // 16 bit samples, host byte order: keep the high byte
            unsigned short v;
            memcpy(&v, src + (x * 3 + k) * 2, 2);
            c[k] = v >> 8;
          }
        }
        dst->r = c[0];
        dst->g = c[1];
        dst->b = c[2];
        dst->m = 255;
      }
    }
    return ras;
  }

  if (p.format == SANE_FRAME_GRAY && (depth == 8 || depth == 16 || depth == 1)) {
    TRasterGR8P ras(lx, ly);
    for (int y = 0; y < ly; ++y) {
      const unsigned char *src = row(y);
      TPixelGR8 *dst           = ras->pixels(ly - 1 - y);
      for (int x = 0; x < lx; ++x) {
        int v;
        if (depth == 1)
          v = ((src[x >> 3] >> (7 - (x & 7))) & 1) ? 0 : 255;  // 1 == black
        else if (depth == 8)
          v = src[x];
        else
        {
          unsigned short w;
          memcpy(&w, src + x * 2, 2);
          v = w >> 8;
        }
        if (softwareThreshold && depth != 1) v = v < threshold ? 0 : 255;
        dst[x].value = (unsigned char)v;
      }
    }
    return ras;
  }
  return TRasterP();  // separate colour planes, or an unknown depth
}

}  // namespace

//-----------------------------------------------------------------------------

TScannerSane::TScannerSane() {}

//-----------------------------------------------------------------------------

TScannerSane::~TScannerSane() {
  if (saneInitialized) {
    sane_exit();
    saneInitialized = false;
  }
}

//-----------------------------------------------------------------------------

QStringList TScannerSane::listDevices() {
  QStringList names;
  for (const DeviceInfo &d : queryDevices()) names << d.name;
  return names;
}

//-----------------------------------------------------------------------------

QString TScannerSane::currentDevice() {
  return QSettings().value(SettingsKey).toString();
}

//-----------------------------------------------------------------------------

void TScannerSane::setCurrentDevice(const QString &name) {
  QSettings().setValue(SettingsKey, name);
}

//-----------------------------------------------------------------------------

bool TScannerSane::isDeviceAvailable() { return !queryDevices().isEmpty(); }

//-----------------------------------------------------------------------------

bool TScannerSane::isDeviceSelected() { return deviceSelected; }

//-----------------------------------------------------------------------------

// Resolves the chosen device (or the first one found) and sets the display
// name shown in the Scan Settings popup.
void TScannerSane::selectDevice() {
  QList<DeviceInfo> devices = queryDevices();
  if (devices.isEmpty()) throw TException("No SANE scanner is available");

  QString wanted = currentDevice();
  const DeviceInfo *found = nullptr;
  for (const DeviceInfo &d : devices)
    if (d.name == wanted) found = &d;
  if (!found) {
    found = &devices.first();
    setCurrentDevice(found->name);
  }

  deviceSelected = true;
  setName((found->vendor + " " + found->model).trimmed() + " (" +
          found->name + ")");
}

//-----------------------------------------------------------------------------

// Reads the scanner's options and fills in what it supports: pixel types,
// resolution range and maximum paper size. Brightness, contrast and the
// feeder are not exposed yet (phase 3).
void TScannerSane::updateParameters(TScannerParameters &param) {
  Device dev(deviceToOpen());
  SANE_Handle h = dev.handle();
  Caps caps     = probe(h);

  bool gray  = !caps.modeGray.isEmpty();
  bool color = !caps.modeColor.isEmpty();
  // black & white is made from a gray scan (software threshold) when the
  // device has no usable lineart mode of its own
  bool bw = gray || !caps.modeBW.isEmpty();
  param.setSupportedTypes(bw, gray, color);

  double dpi = getNumeric(h, caps.resolution, 100);
  if (caps.tlx.valid() && caps.tly.valid() && caps.brx.valid() &&
      caps.bry.valid()) {
    double w = deviceToMm(caps.brx, caps.brx.max, dpi) -
               deviceToMm(caps.tlx, caps.tlx.min, dpi);
    double hgt = deviceToMm(caps.bry, caps.bry.max, dpi) -
                 deviceToMm(caps.tly, caps.tly.min, dpi);
    param.setMaxPaperSize(w, hgt);
  }
  param.updatePaperFormat();

  if (caps.resolution.valid()) {
    double def = std::min(std::max(100.0, caps.resolution.min),
                          caps.resolution.max);
    m_dpi = TScanParam((float)caps.resolution.min, (float)caps.resolution.max,
                       (float)def, 1.f);
    param.m_dpi.update(m_dpi);
  } else
    param.m_dpi.m_supported = false;

  // the threshold is applied by OpenToonz on a gray scan (or by the device in
  // its own lineart mode); both use the 0..255 scale of the settings popup
  param.m_threshold.update(TScanParam(0.f, 255.f, 127.f, 1.f));

  // brightness is shown on a 0..255 scale and mapped onto the device range.
  // There is no contrast control in the settings popup, so it is not offered.
  if (caps.brightness.valid())
    param.m_brightness.update(TScanParam(0.f, 255.f, 127.f, 1.f));
  else
    param.m_brightness.m_supported = false;
  param.m_contrast.m_supported = false;

  // automatic document feeder
  if (!caps.srcAdf.isEmpty())
    param.m_paperFeeder.update(TScanParam(0.f, 1.f, 0.f, 1.f));
  else
    param.m_paperFeeder.m_supported = false;
}

//-----------------------------------------------------------------------------

void TScannerSane::acquire(const TScannerParameters &params, int paperCount) {
  TRectD scanArea = params.getScanArea();
  if (scanArea.isEmpty())
    throw TException("Scan area is empty, select a paper size");

  Device dev(deviceToOpen());
  SANE_Handle h = dev.handle();
  Caps caps     = probe(h);

  // paper source: flatbed, or the feeder (never for a preview)
  const bool useFeeder =
      params.isPaperFeederEnabled() && !caps.srcAdf.isEmpty() &&
      !params.isPreview();
  if (caps.sourceIndex >= 0) {
    const QString &src = useFeeder ? caps.srcAdf : caps.srcFlatbed;
    if (!src.isEmpty()) setStringOption(h, caps, caps.sourceIndex, src);
  }
  // The feeder shows a modal progress dialog with a Cancel button, so events
  // must be processed while scanning for it to react. A flatbed scan has no
  // dialog and the rest of the UI must not be re-entered, so it just blocks.
  auto pump = [useFeeder]() {
    if (useFeeder) QCoreApplication::processEvents();
  };

  // pixel type
  QString mode;
  bool softwareThreshold = false;
  switch (params.getScanType()) {
  case TScannerParameters::BW:
    if (!caps.modeGray.isEmpty()) {
      mode              = caps.modeGray;
      softwareThreshold = true;
    } else
      mode = caps.modeBW;
    break;
  case TScannerParameters::GR8:
    mode = caps.modeGray;
    break;
  case TScannerParameters::RGB24:
    mode = caps.modeColor;
    break;
  default:
    throw TException("Unknown scanner mode");
  }
  if (mode.isEmpty() || caps.modeIndex < 0)
    throw TException("The scanner does not support the selected scan mode");
  setStringOption(h, caps, caps.modeIndex, mode);

  // resolution (the device may round it)
  double dpi = params.m_dpi.m_supported ? params.m_dpi.m_value : 100;
  if (caps.resolution.valid() && params.m_dpi.m_supported) {
    setNumeric(h, caps, caps.resolution, dpi);
    dpi = getNumeric(h, caps.resolution, dpi);
  }

  // area, in mm. A preview covers the whole paper; a real scan only the crop
  // box, which is later put back in a canvas of the paper size.
  TRectD cropBox = params.getCropBox();
  TRectD region  = (params.isPreview() || cropBox.isEmpty()) ? scanArea : cropBox;
  setNumeric(h, caps, caps.tlx, mmToDevice(caps.tlx, region.x0, dpi));
  setNumeric(h, caps, caps.tly, mmToDevice(caps.tly, region.y0, dpi));
  setNumeric(h, caps, caps.brx, mmToDevice(caps.brx, region.x1, dpi));
  setNumeric(h, caps, caps.bry, mmToDevice(caps.bry, region.y1, dpi));

  // brightness (left at the device default when the slider is centred) and,
  // in the device's own lineart mode, the threshold
  if (params.m_brightness.m_supported && caps.brightness.valid() &&
      std::fabs(params.m_brightness.m_value - 127.5) > 1.0)
    setNumeric(h, caps, caps.brightness,
               fromUiRange(caps.brightness, params.m_brightness.m_value));
  if (params.getScanType() == TScannerParameters::BW && !softwareThreshold &&
      caps.threshold.valid())
    setNumeric(h, caps, caps.threshold,
               fromUiRange(caps.threshold, params.m_threshold.m_value));

  for (int i = 0; i < paperCount; ++i) {
    if (isScanningCanceled()) break;

    pump();
    if (isScanningCanceled()) break;

    SANE_Status st = sane_start(h);
    if (st == SANE_STATUS_NO_DOCS) {
      if (i == 0) throw TException("Nothing to scan: " + describe(st));
      break;  // feeder emptied before paperCount sheets
    }
    if (st != SANE_STATUS_GOOD)
      throw TException(std::string("Cannot start scan: ") + describe(st));

    Image img;
    st = sane_get_parameters(h, &img.p);
    if (st != SANE_STATUS_GOOD ||
        (img.p.format != SANE_FRAME_GRAY && img.p.format != SANE_FRAME_RGB)) {
      sane_cancel(h);
      throw TException(
          "The scanner delivers an unsupported image format (separate colour "
          "planes)");
    }

    // read until EOF; the height may be unknown (lines == -1)
    std::vector<unsigned char> chunk(1 << 16);
    for (;;) {
      pump();
      if (isScanningCanceled()) {
        sane_cancel(h);
        return;
      }
      SANE_Int len = 0;
      st           = sane_read(h, chunk.data(), (SANE_Int)chunk.size(), &len);
      img.data.insert(img.data.end(), chunk.begin(), chunk.begin() + len);
      if (st == SANE_STATUS_EOF) break;
      if (st != SANE_STATUS_GOOD) {
        sane_cancel(h);
        throw TException(std::string("Error reading image data: ") +
                         describe(st));
      }
    }
    sane_cancel(h);

    int received = img.p.bytes_per_line > 0
                       ? (int)(img.data.size() / img.p.bytes_per_line)
                       : 0;
    img.lines    = img.p.lines > 0 ? std::min(img.p.lines, received) : received;

    TRasterP ras =
        convert(img, softwareThreshold, params.m_threshold.m_value);
    if (!ras) throw TException("The scanned image format is not supported");

    // put a cropped scan back at its place on the full paper
    if (!params.isPreview() && !cropBox.isEmpty() && cropBox != scanArea) {
      int w = std::max((int)std::floor(scanArea.getLx() / 25.4 * dpi),
                       ras->getLx());
      int hh = std::max((int)std::floor(scanArea.getLy() / 25.4 * dpi),
                        ras->getLy());
      int offX = (int)std::lround((cropBox.x0 - scanArea.x0) / 25.4 * dpi);
      int offY = (int)std::lround((cropBox.y0 - scanArea.y0) / 25.4 * dpi);
      const int x0 = std::max(0, offX);
      const int y0 = std::max(0, hh - offY - ras->getLy());  // rows from bottom
      TRaster32P src32(ras);
      TRasterGR8P srcGR8(ras);
      TRasterP canvas;
      if (src32) {
        TRaster32P c(w, hh);
        c->fill(TPixel32::White);
        for (int y = 0; y < src32->getLy() && y0 + y < hh; ++y)
          memcpy((void *)(c->pixels(y0 + y) + x0), (const void *)src32->pixels(y),
                 sizeof(TPixel32) *
                     std::min(src32->getLx(), w - x0));
        canvas = c;
      } else {
        TRasterGR8P c(w, hh);
        c->fill(TPixelGR8::White);
        for (int y = 0; y < srcGR8->getLy() && y0 + y < hh; ++y)
          memcpy((void *)(c->pixels(y0 + y) + x0), (const void *)srcGR8->pixels(y),
                 sizeof(TPixelGR8) *
                     std::min(srcGR8->getLx(), w - x0));
        canvas = c;
      }
      ras = canvas;
    }

    TRasterImageP rasImg(ras);
    rasImg->setDpi(dpi, dpi);
    rasImg->setSavebox(ras->getBounds());
    notifyImageDone(rasImg);

    // feeder: the next sheet is fetched automatically; flatbed: ask the user
    if (useFeeder)
      notifyAutomaticallyNextPaper();
    else if (paperCount - i > 1)
      notifyNextPaper();
  }
}
