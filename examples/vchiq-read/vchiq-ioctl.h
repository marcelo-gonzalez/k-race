// below from drivers/staging/vc04_services/

enum vchiq_reason {
	VCHIQ_SERVICE_OPENED,         /* service, -, -             */
	VCHIQ_SERVICE_CLOSED,         /* service, -, -             */
	VCHIQ_MESSAGE_AVAILABLE,      /* service, header, -        */
	VCHIQ_BULK_TRANSMIT_DONE,     /* service, -, bulk_userdata */
	VCHIQ_BULK_RECEIVE_DONE,      /* service, -, bulk_userdata */
	VCHIQ_BULK_TRANSMIT_ABORTED,  /* service, -, bulk_userdata */
	VCHIQ_BULK_RECEIVE_ABORTED    /* service, -, bulk_userdata */
};

enum vchiq_status {
	VCHIQ_ERROR   = -1,
	VCHIQ_SUCCESS = 0,
	VCHIQ_RETRY   = 1
};

struct vchiq_header {
	/* The message identifier - opaque to applications. */
	int msgid;

	/* Size of message data. */
	unsigned int size;

	char data[0];           /* message */
};

typedef enum vchiq_status (*vchiq_callback)(enum vchiq_reason,
					    struct vchiq_header *,
					    unsigned int, void *);

struct vchiq_service_params {
	int fourcc;
	vchiq_callback callback;
	void *userdata;
	short version;       /* Increment for non-trivial changes */
	short version_min;   /* Update for incompatible changes */
};

struct vchiq_create_service {
	struct vchiq_service_params params;
	int is_open;
	int is_vchi;
	unsigned int handle;       /* OUT */
};

#define VCHIQ_IOC_MAGIC 0xc4
#define VCHIQ_IOC_CREATE_SERVICE \
	_IOWR(VCHIQ_IOC_MAGIC, 2, struct vchiq_create_service)
