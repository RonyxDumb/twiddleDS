#!/bin/sh
# Builds TwiddlefingerDS.nds with the official devkitPro docker image.
docker run --rm -v "$(pwd)":/src -w /src devkitpro/devkitarm make "$@"
