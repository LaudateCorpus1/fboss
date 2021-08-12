# CMake to build libraries and binaries from fboss/lib/SnapshotManager

# In general, libraries and binaries in fboss/foo/bar are built by
# cmake/FooBar.cmake

add_library(snapshot_manager
  fboss/lib/SnapshotManager.cpp
)

target_link_libraries(snapshot_manager
  FBThrift::thriftcpp2
  fboss_cpp2
  phy_cpp2
  alert_logger
)
