# The built-in modules written in tz, one folder per topic, go next to tz into lib/, side by
# side, where the loader reads them (compiler/load/loader/loader.cpp); copied again whenever
# they change.
file(GLOB_RECURSE TZ_LIBRARY CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.tz)
foreach(file ${TZ_LIBRARY})
  get_filename_component(name ${file} NAME)
  configure_file(${file} ${TZ_OUTPUT_DIR}/lib/${name} COPYONLY)
endforeach()
