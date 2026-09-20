# data/

Nearly empty on purpose. [`contrib/`](contrib/README.md) holds mappings
people have recorded for pads the database does not know, and the build
compiles them in on top of the database; see there if you own one.

The build downloads `gamecontrollerdb.txt` into the build directory at
configure time and compiles the lines for the target platform into the
library. Nothing needs to be here for that to work.

Drop a copy of `gamecontrollerdb.txt` in this directory if you want a build
that never reaches the network and still has mappings — it is the last
fallback the build looks at, after `GPPLUS_CONTROLLER_DB_FILE` and the
downloaded copy. Configure with `-DGPPLUS_DOWNLOAD_CONTROLLER_DB=OFF` to
make that the only source.

The file is from [SDL_GameControllerDB][db] and is zlib-licensed; see
`LICENSE` at the root for the attribution that has to travel with it.

    curl -L -o data/gamecontrollerdb.txt \
      https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt

[db]: https://github.com/mdqinc/SDL_GameControllerDB
