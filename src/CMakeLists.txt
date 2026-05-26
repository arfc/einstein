### DO NOT DELETE THIS COMMENT: INSERT_ARCHETYPES_HERE ###
USE_CYCLUS("einstein" "ads")
USE_CYCLUS("einstein" "us_inventory")

INSTALL_CYCLUS_MODULE("einstein" "")

# install header files
FILE(GLOB h_files "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
