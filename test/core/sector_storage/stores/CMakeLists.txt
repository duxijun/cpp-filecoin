#
# Copyright Soramitsu Co., Ltd.All Rights Reserved.
# SPDX - License - Identifier : Apache - 2.0
#

addtest(sector_index_test
        index_test.cpp)

target_link_libraries(sector_index_test
        sector_index
        )

addtest(local_store_test
        local_store_test.cpp)

target_link_libraries(local_store_test
        base_fs_test
        store
        fuhon_host
        )


