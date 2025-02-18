// Copyright (c) 2018-present Alibaba Inc. All Rights Reserved.
// Author:
//   Junquan Chen <jianming.cjq@alipay.com>

#ifndef OB_DIRECT_LOAD_MULTIPLE_HEAP_TABLE_MAP_H_
#define OB_DIRECT_LOAD_MULTIPLE_HEAP_TABLE_MAP_H_

#include "lib/hash/ob_hashmap.h"
#include "common/ob_tablet_id.h"
#include "storage/direct_load/ob_direct_load_external_multi_partition_row.h"
#include "storage/direct_load/ob_direct_load_multi_map.h"

namespace oceanbase
{
namespace storage
{

class ObDirectLoadMultipleHeapTableMap
{
  typedef ObDirectLoadConstExternalMultiPartitionRow RowType;
  typedef common::ObTabletID KeyType;
  typedef common::ObArray<const RowType *> BagType;
  typedef common::hash::ObHashMap<const KeyType, BagType *, common::hash::NoPthreadDefendMode> MapType;
public:
  ObDirectLoadMultipleHeapTableMap(int64_t mem_limit);
  virtual ~ObDirectLoadMultipleHeapTableMap() {}

  int init();
  int add_row(const KeyType &key, const RowType &row);
  int get_all_key_sorted(common::ObArray<KeyType> &key_array);
  int get(const KeyType &key, BagType &out_bag) {
    return tablet_map_.get(key, out_bag);
  }

private:
  DISALLOW_COPY_AND_ASSIGN(ObDirectLoadMultipleHeapTableMap);
  int deep_copy_row(const RowType &row, RowType *&result_row);

private:
  // data members
  ObDirectLoadMultiMapNoLock<KeyType, const RowType *> tablet_map_;
  common::ObArenaAllocator allocator_;
  int64_t mem_limit_;
};

}
}

#endif /* OB_DIRECT_LOAD_MULTIPLE_HEAP_TABLE_MAP_H_ */
