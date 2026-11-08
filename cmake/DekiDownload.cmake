# A release download, tried a few times. GitHub's release downloads answer an
# HTTP error now and then, and a single one stopped the whole configure: three
# CI runs in one day failed on it. `out_status` is file(DOWNLOAD)'s STATUS from
# the last try.
#
# The hash is checked here rather than with EXPECTED_HASH: file(DOWNLOAD) makes
# a failed download with EXPECTED_HASH a CMake error of its own, which fails
# the configure even when a later try succeeds.
function(deki_download url file sha256 out_status)
    set(_attempts 5)
    foreach(_attempt RANGE 1 ${_attempts})
        file(DOWNLOAD "${url}" "${file}" STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        if(_code EQUAL 0)
            file(SHA256 "${file}" _hash)
            if(_hash STREQUAL "${sha256}")
                break()
            endif()
            set(_status "1;SHA256 is ${_hash}, expected ${sha256}")
        endif()
        if(_attempt EQUAL _attempts)
            break()
        endif()
        list(GET _status 1 _message)
        math(EXPR _wait "${_attempt} * 5")
        message(STATUS "Deki: ${url}: ${_message}; trying again in ${_wait} s")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep ${_wait})
    endforeach()
    set(${out_status} "${_status}" PARENT_SCOPE)
endfunction()
