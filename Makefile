PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=dodo
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# ---- dodoc standalone CLI (no DuckDB dependency) ----
DODOC_BUILD_DIR := build/dodoc
DODOC_SOURCES := src/cli/dodoc.cpp src/core/dodo_core.cpp duckdb-dta/src/dta_reader.cpp
DODOC_HEADERS := src/core/dodo_core.hpp src/core/string_utils.hpp duckdb-dta/src/include/dta_reader.hpp
CXX ?= c++
CXXFLAGS ?= -O2 -std=c++17

.PHONY: dodoc dodoc-install dodoc-clean

dodoc: $(DODOC_BUILD_DIR)/dodoc

$(DODOC_BUILD_DIR)/dodoc: $(DODOC_SOURCES) $(DODOC_HEADERS)
	@mkdir -p $(DODOC_BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc/core -Iduckdb-dta/src/include -o $@ $(DODOC_SOURCES)

dodoc-install: dodoc
	install -m 755 $(DODOC_BUILD_DIR)/dodoc /usr/local/bin/dodoc

dodoc-clean:
	rm -rf $(DODOC_BUILD_DIR)

# ---- E2E tests across client interfaces ----
.PHONY: e2e e2e-cli e2e-python e2e-r e2e-node

e2e: e2e-cli e2e-python e2e-r e2e-node

e2e-cli:
	bash test/e2e/test_cli.sh

e2e-python:
	uv run --python 3.13 --with 'duckdb==1.5.2' --with pyyaml test/e2e/test_python.py

RSCRIPT ?= Rscript
R_DUCKDB_VERSION ?= $(shell $(RSCRIPT) -e 'library(DBI);library(duckdb);con<-dbConnect(duckdb());cat(gsub("^v","",dbGetQuery(con,"SELECT version()")[[1]]));dbDisconnect(con,shutdown=TRUE)' 2>/dev/null || echo "1.5.0")
R_EXT_PATH ?= $(PROJ_DIR)build/r_$(R_DUCKDB_VERSION)/extension/dodo/dodo.duckdb_extension

e2e-r: build/r_$(R_DUCKDB_VERSION)/extension/dodo/dodo.duckdb_extension
	DODO_EXT_PATH=$(R_EXT_PATH) $(RSCRIPT) test/e2e/test_r.R $(PROJ_DIR)

NODE_DUCKDB_VERSION ?= $(shell node -e 'const{DuckDBInstance:D}=require("@duckdb/node-api");D.create(":memory:").then(i=>i.connect()).then(c=>c.run("SELECT version()")).then(r=>{console.log(r.getChunk(0).getColumnVector(0).getItem(0).replace("v",""))})' 2>/dev/null || echo "1.5.2")
NODE_EXT_PATH ?= $(PROJ_DIR)build/node_$(NODE_DUCKDB_VERSION)/extension/dodo/dodo.duckdb_extension

e2e-node: build/node_$(NODE_DUCKDB_VERSION)/extension/dodo/dodo.duckdb_extension
	DODO_EXT_PATH=$(NODE_EXT_PATH) node test/e2e/test_node.js

# ---- Build extension for specific DuckDB versions ----
DUCKDB_MAIN_VERSION := $(shell git -C duckdb describe --tags 2>/dev/null | sed 's/^v//')

build/%/extension/dodo/dodo.duckdb_extension: src/extension/dodo_extension.cpp src/core/dodo_core.cpp
	$(eval TARGET_VERSION := $(patsubst build/%/extension/dodo/dodo.duckdb_extension,%,$@))
	$(eval DUCKDB_TAG := $(shell echo $(TARGET_VERSION) | sed 's/^r_//' | sed 's/^node_//'))
	cd duckdb && git checkout v$(DUCKDB_TAG)
	rm -rf build/$(TARGET_VERSION)
	mkdir -p build/$(TARGET_VERSION)
	cmake -DEXTENSION_STATIC_BUILD=1 -DDUCKDB_EXTENSION_CONFIGS="$(PROJ_DIR)extension_config.cmake" \
		-DCMAKE_BUILD_TYPE=Release -S ./duckdb/ -B build/$(TARGET_VERSION) \
		-DUNITTEST_ROOT_DIRECTORY="$(PROJ_DIR)"
	cmake --build build/$(TARGET_VERSION) --config Release --target dodo_loadable_extension
	cd duckdb && git checkout v$(DUCKDB_MAIN_VERSION)
