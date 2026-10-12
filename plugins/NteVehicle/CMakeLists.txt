add_library(NteVehicle SHARED plugin.cpp)
target_link_libraries(NteVehicle PRIVATE Anomaly::sdk)
target_compile_features(NteVehicle PRIVATE cxx_std_20)
set_target_properties(NteVehicle PROPERTIES OUTPUT_NAME plugin PREFIX "")
