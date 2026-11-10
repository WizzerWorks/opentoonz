#pragma once

#ifndef SCANNER_SANE_INCLUDED
#define SCANNER_SANE_INCLUDED

#include "tscanner.h"

#include <QStringList>

// SANE (Scanner Access Now Easy) driver. Built only when libsane is found
// (HAVE_SANE). See doc/MLW_OpenToonz_Scanner-Arch.docx, section 6.
//
// Phase 0 status: device enumeration and selection only. updateParameters()
// and acquire() are stubs that are completed in later phases.
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
