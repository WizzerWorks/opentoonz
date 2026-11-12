#pragma once

#ifndef SCANNER_SANE_INCLUDED
#define SCANNER_SANE_INCLUDED

#include "tscanner.h"

#include <QStringList>

// SANE (Scanner Access Now Easy) driver. Built only when libsane is found
// (HAVE_SANE). See doc/MLW_OpenToonz_Scanner-Proposal.docx, section 6 (design) and doc/MLW_OpenToonz_Scanner-Arch.docx (as built).
//
// Features: device selection, option probing, scans in gray / colour / B&W
// (resolution, area, crop box, preview), brightness and threshold, flatbed
// multi-sheet scans and the automatic document feeder, cancel while scanning.
// Contrast is not offered (the settings popup has no control for it).
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
