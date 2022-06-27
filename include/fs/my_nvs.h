#ifndef ZEPHYR_INCLUDE_FS_MY_NVS_H_
#define ZEPHYR_INCLUDE_FS_MY_NVS_H_


static inline int nvs_init(struct nvs_fs *fs, const char *dev_name) {
	return my_nvs_init(fs, dev_name);
}

static inline ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len) {
	return my_nvs_write(id, data, len);
}

static inline int nvs_delete(struct nvs_fs *fs, uint16_t id) {
	return my_nvs_delete(id);
}

static inline ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t len) {
	return my_nvs_read(id, data, len);
}

static inline ssize_t nvs_calc_free_space(struct nvs_fs *fs) {
	return my_nvs_calc_free_space();
}


#endif /* ZEPHYR_INCLUDE_FS_MY_NVS_H_ */
