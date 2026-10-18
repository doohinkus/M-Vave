# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Build container for non-Windows hosts (see build.sh): PowerShell plus curl,
# running the same build scripts as build.bat on Windows.
FROM mcr.microsoft.com/powershell:debian-12
RUN apt-get update \
    && apt-get install -y --no-install-recommends curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1
