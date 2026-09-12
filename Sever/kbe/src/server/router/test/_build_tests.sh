#!/bin/sh
# ---------------------------------------------------------------------------
# Build the router unit tests (Linux / macOS).
#
#   usage: sh _build_tests.sh
#   exit : 0 = ok, 1 = compile failed
# ---------------------------------------------------------------------------
set -e

TESTDIR=$(cd "$(dirname "$0")" && pwd)
KBE_SRC=$(cd "$TESTDIR/../../.." && pwd)

INC="-I$KBE_SRC -I$KBE_SRC/lib -I$KBE_SRC/lib/dependencies -I$KBE_SRC/lib/dependencies/fmt/include"

cd "$TESTDIR"

echo "[BUILD] router_registry_test"
${CXX:-g++} -std=c++11 -O2 -Wall $INC \
	router_registry_test.cpp ../actor_registry.cpp -o router_registry_test

echo "[BUILD] router_protocol_test"
${CXX:-g++} -std=c++11 -O2 -Wall $INC \
	router_protocol_test.cpp ../actor_registry.cpp -o router_protocol_test

echo "[BUILD] router_mail_test"
${CXX:-g++} -std=c++11 -O2 -Wall $INC \
	router_mail_test.cpp -o router_mail_test

# L3 protocol client: zero KBE dependency, POSIX socket only
echo "[BUILD] router_proto_client"
${CXX:-g++} -std=c++11 -O2 -Wall $INC \
	router_proto_client.cpp -o router_proto_client

echo "[BUILD] done"
