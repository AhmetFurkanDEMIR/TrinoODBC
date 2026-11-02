// Portable rules of the Windows setup dialog (see core/platform_setup.h).
#include "core/platform_setup.h"

#include "util/strings.h"

namespace trino_odbc {

DerivedTransport deriveTransport(const KeyValueMap& values, bool honourAuthKey, bool honourSslKey) {
  ConnectionConfig probe;
  for (const auto& [key, value] : values) {
    const std::string canonical = ConnectionConfig::canonicalKey(key);
    if (canonical.empty() || iequals(canonical, "DSN") || iequals(canonical, "Driver")) continue;
    if (!honourAuthKey && iequals(canonical, "AuthenticationType")) continue;
    if (!honourSslKey && iequals(canonical, "SSL")) continue;
    std::string ignored;
    probe.applyKeyValue(key, value, &ignored);  // malformed values are reported by the dialog's validation
  }
  probe.finalize();
  DerivedTransport out;
  out.auth = probe.auth;
  out.ssl = probe.ssl;
  return out;
}

void applyTransportChoice(KeyValueMap& values, const TransportChoice& choice) {
  // Authentication first: the implied SSL value depends on the (explicit or implied) auth type.
  const std::string impliedAuth = authTypeName(deriveTransport(values, false, false).auth);
  if (!choice.authExplicit && !choice.authTouched && iequals(choice.auth, impliedAuth)) {
    values.erase("AuthenticationType");
  } else {
    values["AuthenticationType"] = choice.auth;
  }
  const bool impliedSsl = deriveTransport(values, true, false).ssl;
  if (!choice.sslExplicit && !choice.sslTouched && choice.ssl == impliedSsl) {
    values.erase("SSL");
  } else {
    values["SSL"] = choice.ssl ? "1" : "0";
  }
}

KeyValueMap promptDialogValues(const ConnectionConfig& config) {
  KeyValueMap values = ConnectionConfig::parseConnectionString(config.toConnectionString(true));
  if (!config.sslExplicit) values.erase("SSL");
  if (!config.authExplicit) values.erase("AuthenticationType");
  return values;
}

bool needsOverwriteConfirmation(std::string_view originalDsn, std::string_view targetDsn, bool targetExists) {
  if (!targetExists || targetDsn.empty()) return false;
  // Data source names are case-insensitive in the installer API (registry key / INI section names).
  return originalDsn.empty() || !iequals(originalDsn, targetDsn);
}

}  // namespace trino_odbc
