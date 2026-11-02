#pragma once
// Portable part of the Windows DSN setup / connection dialog (src/platform/win/setup.cpp): how the
// dialog treats the SSL and AuthenticationType keys, whose defaults ConnectionConfig::finalize()
// derives from other keys. Kept out of the Win32 file so the rules are unit-tested on every platform.
#include "core/config.h"

#include <string>
#include <string_view>

namespace trino_odbc {

/// Transport settings ConnectionConfig::finalize() ends up with for a map of connection keys.
struct DerivedTransport {
  AuthType auth = AuthType::None;
  bool ssl = false;
};

/// Runs finalize() over `values` (canonical or alias keys; malformed values are ignored, validation
/// reports them elsewhere). With `honourAuthKey` / `honourSslKey` false the respective key is treated
/// as absent, which yields the *implied* value.
DerivedTransport deriveTransport(const KeyValueMap& values, bool honourAuthKey, bool honourSslKey);

/// What the dialog shows and what the user did with the SSL / authentication controls.
struct TransportChoice {
  std::string auth;           // AuthenticationType value of the selected combo entry
  bool authExplicit = false;  // AuthenticationType was stored before the dialog opened
  bool authTouched = false;   // the user operated the combo
  bool ssl = false;           // state of the SSL checkbox
  bool sslExplicit = false;   // SSL was stored before the dialog opened
  bool sslTouched = false;    // the user clicked the checkbox
};

/// Writes the dialog's SSL / AuthenticationType back into `values`. A key that was implicit stays
/// implicit (is erased) unless the user operated its control or the shown value differs from what
/// finalize() derives from the other keys; otherwise saving an untouched dialog would turn
/// "Port=443 + PWD" (https, LDAP) into an explicit SSL=0 / AuthenticationType=None.
void applyTransportChoice(KeyValueMap& values, const TransportChoice& choice);

/// Values the connection prompt starts from: everything toConnectionString(true) reports for a
/// not-yet-finalized configuration, minus SSL / AuthenticationType when they were not given
/// explicitly (toConnectionString always emits SSL, which would make the default explicit).
KeyValueMap promptDialogValues(const ConnectionConfig& config);

/// ConfigDSN with a parent window: true when saving under `targetDsn` would replace a different,
/// existing data source (SQLWriteDSNToIni silently deletes it), so the user must confirm first.
/// `originalDsn` is the DSN being edited ("" when adding); keeping its name, in any letter case, is
/// not an overwrite.
bool needsOverwriteConfirmation(std::string_view originalDsn, std::string_view targetDsn, bool targetExists);

}  // namespace trino_odbc
