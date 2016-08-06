SET(MATLAB_MEX_FOUND 0)

if(_WIN64)
    set(MATLAB_ARCH win64)
elseif(_WIN32)
    set(MATLAB_ARCH win32)
else()
    set(MATLAB_ARCH glnxa64)
endif()

SET(MATLAB_MEX_INCLUDE_DIR_PATHS ${MATLAB_ROOT}/extern/include)

SET(MATLAB_MEX_LIBRARY_PATHS   ${MATLAB_ROOT}/bin/${MATLAB_ARCH})

find_path(MATLAB_MEX_INCLUDE_DIR NAMES mex.h PATHS ${MATLAB_MEX_INCLUDE_DIR_PATHS})
find_library(MATLAB_LIBMEX NAMES mex libmex PATHS ${MATLAB_MEX_LIBRARY_PATHS})
find_library(MATLAB_LIBMAT NAMES mat libmax PATHS ${MATLAB_MEX_LIBRARY_PATHS})
find_library(MATLAB_LIBMENG NAMES eng libeng PATHS ${MATLAB_MEX_LIBRARY_PATHS})
find_library(MATLAB_LIBMX NAMES mx libmx PATHS ${MATLAB_MEX_LIBRARY_PATHS})

mark_as_advanced(MATLAB_LIBMEX MATLAB_LIBMAT MATLAB_LIBMENG MATLAB_LIBMX MATLAB_MEX_INCLUDE_DIR)

set(MATLAB_MEX_INCLUDE_DIRS ${MATLAB_MEX_INCLUDE_DIR})
set(MATLAB_MEX_LIBRARIES ${MATLAB_LIBMEX} ${MATLAB_LIBMAT} ${MATLAB_LIBMENG} ${MATLAB_LIBMX})


include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MATLAB_MEX  DEFAULT_MSG
                                  MATLAB_ROOT MATLAB_MEX_LIBRARIES MATLAB_MEX_INCLUDE_DIR)
