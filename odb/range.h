#ifndef ODB_RANGE_H
#define ODB_RANGE_H

/* Fill `buffer` with exactly `length` bytes beginning at `offset`. */
typedef int odb_range_read_fn(void *data, uint64_t offset, size_t length,
			     void *buffer);
typedef void odb_range_release_fn(void *data);

#endif /* ODB_RANGE_H */
