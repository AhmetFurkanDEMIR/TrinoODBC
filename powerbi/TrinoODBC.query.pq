// Use this file to write queries to test your data connector (Power Query SDK: "Evaluate current
// power query file"). Set the credential for the data source path first ("Set credential").
//
// The default targets a local development cluster without authentication on port 8080 with the
// tpch catalog: choose the Anonymous credential (the Trino user is then "powerbi"). Other targets:
//   TrinoODBC.Contents("http://localhost:18080", "tpch", [Schema = "tiny"])   // plain container of tests/docker
//   TrinoODBC.Contents("http://localhost:18090", "mock", [Schema = "demo"])   // tests/mock_trino/mock_trino.py (default port)
//   TrinoODBC.Contents("trino.corp.example.com:8443", "hive")                 // real cluster
// For a real cluster use a Username/Password (LDAP), Windows (Kerberos) or Key (JWT) credential.
// The http:// prefix sets SSL=0, which also overrides the "Encrypt connection" option.
let
    result = TrinoODBC.Contents("http://localhost:8080", "tpch", [Schema = "tiny"])
in
    result
