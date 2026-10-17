#include <fg.h>
#include <cstdio>

// Link-time test double: even a broken CLI cannot create a native HTTP transport.
extern "C" fg_status fg_transport_default(fg_transport*, fg_error* error)
{
	std::fputs("DEFAULT_NETWORK_TRANSPORT_BLOCKED\n", stderr);
	return fg_fail(error, FG_INVALID, "DEFAULT_NETWORK_TRANSPORT_BLOCKED");
}

extern "C" void fg_transport_default_free(fg_transport*) {}
