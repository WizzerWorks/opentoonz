#include "tscannersane.h"
#include "texception.h"

#include <QSettings>

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

// Phase 0: nothing to negotiate yet. Phase 2 reads the option descriptors.
void TScannerSane::updateParameters(TScannerParameters &param) {
  param.setSupportedTypes(false, true, true);
}

//-----------------------------------------------------------------------------

void TScannerSane::acquire(const TScannerParameters &, int) {
  throw TException("Scanning with SANE is not implemented yet");
}
