#pragma once

#ifndef SCANNER_SANE_INCLUDED
#define SCANNER_SANE_INCLUDED

#include "tscanner.h"

#include <QStringList>

// SANE (Scanner Access Now Easy) driver. Built only when libsane is found
// (HAVE_SANE). See doc/MLW_OpenToonz_Scanner-Arch.docx, section 6.
//
// Phase 2 status: device selection, option probing and single-sheet scans
// (gray/colour/B&W, resolution, area, crop box). Brightness, contrast, the
// feeder (ADF) and cancel during a scan come in phase 3.
class TScannerSane final : public TScanner {
public:
  TScannerSane();
  ~TScannerSane();

  bool isDeviceAvailable() override;
  bool isDeviceSelected() override;
  void selectDevice() override;

  void updateParameters(TScannerParameters &) override;
  void acquire(const TScannerParameters &param, int paperCount) override;

  // Names (SANE device identifiers) of all scanners currently visible,
  // including network ones. Empty if SANE cannot be initialised.
  static QStringList listDevices();

  // The device chosen by the user, persisted per machine in QSettings
  // ("CurrentSaneDevice"). Empty means "use the first one found".
  static QString currentDevice();
  static void setCurrentDevice(const QString &name);
};

#endif
