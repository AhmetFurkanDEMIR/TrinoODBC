// Resource identifiers shared by trinoodbc.rc (icon, dialog template, VERSIONINFO) and setup.cpp.
// This header is included by the resource compiler, so it must contain nothing but preprocessor
// definitions.
#ifndef TRINO_ODBC_PLATFORM_WIN_RESOURCE_H
#define TRINO_ODBC_PLATFORM_WIN_RESOURCE_H

// ---- version block (keep in sync with kDriverVersion in src/platform.h) ----------------------
#define TRINO_ODBC_RC_FILEVERSION        1,0,0,0
#define TRINO_ODBC_RC_PRODUCTVERSION     1,0,0,0
#define TRINO_ODBC_RC_FILEVERSION_STR    "1.0.0.0"
#define TRINO_ODBC_RC_PRODUCTVERSION_STR "1.0.0"
#define TRINO_ODBC_RC_PRODUCT_NAME       "Trino ODBC Driver"
#define TRINO_ODBC_RC_FILE_DESCRIPTION   "Trino ODBC Driver (ODBC 3.8, Unicode)"
#define TRINO_ODBC_RC_ORIGINAL_FILENAME  "trinoodbc.dll"
#define TRINO_ODBC_RC_INTERNAL_NAME      "trinoodbc"
#define TRINO_ODBC_RC_COMPANY_NAME       "Trino ODBC project"
#define TRINO_ODBC_RC_COPYRIGHT          "Licensed under the Apache License, Version 2.0"
#define TRINO_ODBC_RC_TRADEMARKS         "Trino and the Trino logo are trademarks of the Trino Software Foundation. This driver is an independent project, not affiliated with or endorsed by it."

// ---- icons ------------------------------------------------------------------------------------
// The first (lowest-numbered) icon group is the one Explorer and "Add/Remove Programs"
// (DisplayIcon = trinoodbc.dll,0) show; the DSN dialog uses it as its window icon.
#define IDI_TRINOODBC               1

// ---- dialogs ----------------------------------------------------------------------------------
#define IDD_TRINO_DSN_DIALOG        101

// ---- controls of IDD_TRINO_DSN_DIALOG ----------------------------------------------------------
// Data source
#define IDC_DSN_NAME                1001
#define IDC_DESCRIPTION             1002
// Server
#define IDC_HOST                    1003
#define IDC_PORT                    1004
#define IDC_CATALOG                 1005
#define IDC_SCHEMA                  1006
// Authentication
#define IDC_AUTH_TYPE               1007
#define IDC_USER                    1008
#define IDC_PASSWORD                1009
#define IDC_SAVE_PASSWORD           1010
#define IDC_TOKEN                   1011
// TLS
#define IDC_SSL                     1012
#define IDC_SSL_VERIFICATION        1013
#define IDC_CA_FILE                 1014
#define IDC_CLIENT_CERT             1015
#define IDC_CLIENT_KEY              1016
#define IDC_CLIENT_KEY_PASSWORD     1017
// Session
#define IDC_SESSION_PROPERTIES      1018
#define IDC_EXTRA_CREDENTIALS       1019
#define IDC_ROLES                   1020
#define IDC_TIME_ZONE               1021
#define IDC_SOURCE                  1022
// Transfer & logging
#define IDC_SPOOLING                1023
#define IDC_TARGET_RESULT_SIZE      1024
#define IDC_LOG_LEVEL               1025
#define IDC_LOG_PATH                1026
// Buttons / status
#define IDC_TEST_CONNECTION         1027
#define IDC_STATUS                  1028
#define IDC_HINTS                   1029
#define IDC_TIME_ZONE_NOTE          1030
#define IDC_TRADEMARK_NOTE          1031

#ifndef IDC_STATIC
#  define IDC_STATIC (-1)
#endif

#endif  // TRINO_ODBC_PLATFORM_WIN_RESOURCE_H
