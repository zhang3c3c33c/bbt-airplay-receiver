aux_source_directory(. dnssd_src)
set(DIR_SRCS ${dnssd_src})
include_directories(.)
add_library( dnssd 
        STATIC
        ${DIR_SRCS})



if (WIN32)
  target_link_libraries( dnssd iphlpapi ws2_32 )
endif()
