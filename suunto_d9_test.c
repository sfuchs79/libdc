#include <stdio.h>	// fopen, fwrite, fclose

#include "suunto.h"
#include "utils.h"

#define WARNING(expr) \
{ \
	message ("%s:%d: %s\n", __FILE__, __LINE__, expr); \
}

int test_dump_sdm (const char* name)
{
	d9 *device = NULL;

	message ("suunto_d9_open\n");
	int rc = suunto_d9_open (&device, name);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("suunto_d9_read_version\n");
	unsigned char version[4] = {0};
	rc = suunto_d9_read_version (device, version, sizeof (version));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot identify computer.");
		suunto_d9_close (device);
		return rc;
	}

	message ("suunto_d9_read_dives\n");
	rc = suunto_d9_read_dives (device, NULL, NULL);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read dives.");
		suunto_d9_close (device);
		return rc;
	}

	message ("suunto_d9_close\n");
	rc = suunto_d9_close (device);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return SUUNTO_SUCCESS;
}

int test_dump_memory (const char* name, const char* filename)
{
	unsigned char data[SUUNTO_D9_MEMORY_SIZE] = {0};
	d9 *device = NULL;

	message ("suunto_d9_open\n");
	int rc = suunto_d9_open (&device, name);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Error opening serial port.");
		return rc;
	}

	message ("suunto_d9_read_version\n");
	unsigned char version[4] = {0};
	rc = suunto_d9_read_version (device, version, sizeof (version));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot identify computer.");
		suunto_d9_close (device);
		return rc;
	}

	message ("suunto_d9_read_memory\n");
	rc = suunto_d9_read_memory (device, 0x00, data, sizeof (data));
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot read memory.");
		suunto_d9_close (device);
		return rc;
	}

	message ("Dumping data\n");
	FILE* fp = fopen (filename, "wb");
	if (fp != NULL) {
		fwrite (data, sizeof (unsigned char), sizeof (data), fp);
		fclose (fp);
	}

	message ("suunto_d9_close\n");
	rc = suunto_d9_close (device);
	if (rc != SUUNTO_SUCCESS) {
		WARNING ("Cannot close device.");
		return rc;
	}

	return SUUNTO_SUCCESS;
}

const char* errmsg (int rc)
{
	switch (rc) {
	case SUUNTO_SUCCESS:
		return "Success";
	case SUUNTO_ERROR:
		return "Generic error";
	case SUUNTO_ERROR_IO:
		return "Input/output error";
	case SUUNTO_ERROR_MEMORY:
		return "Memory error";
	case SUUNTO_ERROR_PROTOCOL:
		return "Protocol error";
	case SUUNTO_ERROR_TIMEOUT:
		return "Timeout";
	default:
		return "Unknown error";
	}
}

int main(int argc, char *argv[])
{
	message_set_logfile ("D9.LOG");

#ifdef _WIN32
	const char* name = "COM1";
#else
	const char* name = "/dev/ttyS0";
#endif

	if (argc > 1) {
		name = argv[1];
	}

	message ("DEVICE=%s\n", name);

	int a = test_dump_memory (name, "D9.DMP");
	int b = test_dump_sdm (name);

	message ("\nSUMMARY\n");
	message ("-------\n");
	message ("test_dump_memory: %s\n", errmsg (a));
	message ("test_dump_sdm:    %s\n", errmsg (b));

	message_set_logfile (NULL);

	return 0;
}
