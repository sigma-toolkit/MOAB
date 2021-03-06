set( FBiGEOM_SOURCE FBiGeom_MOAB.hpp FBiGeom_MOAB.cpp )
set(
  FBiGEOM_INCLUDE_HEADERS
  FBiGeom.h
  FBiGeom_protos.h
  FBiGeom_f.h
  )

add_library(
  FBiGeomMOAB
  ${FBiGEOM_SOURCE}
  ${FBiGEOM_INCLUDE_HEADERS}
  )

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/src
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/itaps
  ${CMAKE_BINARY_DIR}/itaps
  ${CMAKE_SOURCE_DIR}/itaps/imesh
  ${CMAKE_BINARY_DIR}/itaps/imesh
 )

if ( MOAB_HAVE_MPI )
  include_directories( ${CMAKE_SOURCE_DIR}/src/parallel
                       ${CMAKE_BINARY_DIR}/src/parallel)
endif ()

target_link_libraries( FBiGeomMOAB iMesh MOAB ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES} )

set_target_properties(
  FBiGeomMOAB
  PROPERTIES
  VERSION ${MOAB_VERSION}
  SOVERSION ${MOAB_VERSION_MAJOR}
  )

install(
  TARGETS FBiGeomMOAB
  EXPORT MOABTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
set( MOAB_INSTALL_TARGETS ${MOAB_INSTALL_TARGETS} FBiGeomMOAB)

install(
  FILES ${FBiGEOM_INCLUDE_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )

