string(TIMESTAMP BUILD_TIME "%Y.%m.%d.%H%M")
set(VERSION "v${BUILD_TIME}")
file(WRITE "${OUTPUT_FILE}" "#pragma once\n#define MOLINK_VERSION \"${VERSION}\"\n")
message(STATUS "Version: ${VERSION}")
