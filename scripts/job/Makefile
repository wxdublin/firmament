ROOT_DIR=../..
SUFFIX=scripts/job

include $(ROOT_DIR)/include/Makefile.config
include $(ROOT_DIR)/include/Makefile.common

PBS=base/job_desc.proto base/task_desc.proto base/reference_desc.proto \
		base/resource_vector.proto base/task_final_report.proto \
		base/usage_list.proto base/task_usage_record.proto

protobufs: $(addprefix $(SRC_ROOT_DIR)/, $(PBS))
	protoc -I$(SRC_ROOT_DIR)/ --python_out=. $^
	touch base/__init__.py
