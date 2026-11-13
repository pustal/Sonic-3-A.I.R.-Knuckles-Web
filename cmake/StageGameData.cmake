# StageGameData.cmake
#
# Script mode helper (cmake -P) that builds the game data folder the app ships
# (<app>/game, see src/port/GameFolder.h), in the layout the official release
# packs into its data/*.bin packages, but as plain files:
#   game/data/      the Oxygen engine's data, then Sonic 3 A.I.R.'s over it
#                   (the release's enginedata.bin, gamedata.bin and audiodata.bin),
#                   without the remastered soundtrack (audioremaster.bin), which
#                   players add themselves (README.md explains why)
#   game/scripts/   the game's scripts, compiled by the game at its first start
#   game/config.json  webos/config.json
#   game/oxygenproject.json  webos/oxygenproject.json (the game profile, which
#                   makes the game compile the scripts; see the file)
#
# Variables: AIR_GAME_DIR (Oxygen/sonic3air), AIR_ENGINE_DATA (Oxygen/oxygenengine/data),
#            CONFIG (webos/config.json), PROJECT (webos/oxygenproject.json),
#            OUTPUT (the game folder to create), STAMP.

foreach(required AIR_GAME_DIR AIR_ENGINE_DATA CONFIG PROJECT OUTPUT STAMP)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "StageGameData.cmake needs ${required}")
    endif()
endforeach()

file(REMOVE_RECURSE "${OUTPUT}")
file(MAKE_DIRECTORY "${OUTPUT}/data")

# Engine data first: where both have a file, the game's wins (as its package
# is mounted after the engine's in the release).
file(COPY "${AIR_ENGINE_DATA}/" DESTINATION "${OUTPUT}/data")
file(COPY "${AIR_GAME_DIR}/data/" DESTINATION "${OUTPUT}/data"
     PATTERN "audio/remastered" EXCLUDE)
file(COPY "${AIR_GAME_DIR}/scripts" DESTINATION "${OUTPUT}")
file(COPY_FILE "${CONFIG}" "${OUTPUT}/config.json")
file(COPY_FILE "${PROJECT}" "${OUTPUT}/oxygenproject.json")

file(TOUCH "${STAMP}")
message(STATUS "Game data staged in ${OUTPUT}")
