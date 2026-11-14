cmake_minimum_required(VERSION 3.22)

if(NOT EXISTS "${ANOMALY_RUNTIME_ROOT}/anomaly.ini")
    file(MAKE_DIRECTORY "${ANOMALY_RUNTIME_ROOT}")
    file(COPY "${CMAKE_CURRENT_LIST_DIR}/../config/anomaly.ini"
        DESTINATION "${ANOMALY_RUNTIME_ROOT}")
endif()
