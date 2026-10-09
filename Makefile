# Copyright (c) 2026 Johannes Overmann
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE or copy at https://www.boost.org/LICENSE_1_0.txt)

CXX ?= clang++
CXXSTD ?= -std=c++23
CPPFLAGS ?= -pedantic
BUILD ?= release
CXXFLAGS_COMMON ?= -Wall -Wextra
CXXFLAGS_DEBUG ?= -O0 -g
CXXFLAGS_RELEASE ?= -O3 -DNDEBUG
PYTHON ?= python3

ifeq ($(BUILD),debug)
CXXFLAGS ?= $(CXXFLAGS_COMMON) $(CXXFLAGS_DEBUG)
else ifeq ($(BUILD),release)
CXXFLAGS ?= $(CXXFLAGS_COMMON) $(CXXFLAGS_RELEASE)
else
$(error Unknown BUILD='$(BUILD)', expected debug or release)
endif

BUILDDIR = build-$(BUILD)
SOURCES = $(wildcard src/*.cpp)
OBJECTS = $(SOURCES:%.cpp=$(BUILDDIR)/%.o)
DEPENDS = $(SOURCES:%.cpp=$(BUILDDIR)/%.d)
TARGET = tmacpower

default: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $^ -framework IOKit -framework CoreFoundation -o $@
	@echo "Done."

$(BUILDDIR)/%.o: %.cpp $(BUILDDIR)/%.d
	$(CXX) $(CXXSTD) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR)/%.d: %.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) $(CXXSTD) $(CPPFLAGS) -MM -MQ $@ $< -o $@

clean:
	rm -rf build-* $(TARGET)

test: $(TARGET)
	$(PYTHON) -m unittest discover -s test -v

.PHONY: default clean test

-include $(DEPENDS)
