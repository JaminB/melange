# Applied to civetweb v1.16 by FetchContent (PATCH_COMMAND). Every replacement must match exactly once.
#  - header parser: reject obs-fold, whitespace before ':' and more than MG_MAX_HEADERS headers (instead of ignoring them);
#  - WebSocket: a server rejects unmasked client frames and frames over MG_OASIS_MAX_FRAME before allocating;
#  - mg_oasis_close_after / mg_oasis_abort: let the embedder close a connection it has answered, or abort one.
set(f "${SRC}/src/civetweb.c")
file(READ "${f}" text)
if(text MATCHES "mg_oasis_abort")
    return()
endif()

function(replace_once from to)
    string(FIND "${text}" "${from}" first)
    if(first EQUAL -1)
        message(FATAL_ERROR "civetweb patch: anchor not found: ${from}")
    endif()
    string(LENGTH "${from}" len)
    math(EXPR after "${first} + ${len}")
    string(SUBSTRING "${text}" ${after} -1 rest)
    string(FIND "${rest}" "${from}" second)
    if(NOT second EQUAL -1)
        message(FATAL_ERROR "civetweb patch: anchor not unique: ${from}")
    endif()
    string(REPLACE "${from}" "${to}" text "${text}")
    set(text "${text}" PARENT_SCOPE)
endfunction()

replace_once("		if (dp == *buf) {
			/* End of headers reached. */"
"		if (dp == *buf) {
			if ((*dp == ' ') || (*dp == '\\t')) {
				return -1;
			}
			/* End of headers reached. */")

replace_once("		/* Drop all spaces after header name before : */
		while (*dp == ' ') {
			*dp = 0;
			dp++;
		}"
"		if (*dp == ' ') {
			return -1;
		}")

replace_once("	return num_headers;
}"
"	if (i >= (int)MG_MAX_HEADERS) {
		return -1;
	}
	return num_headers;
}")

replace_once("		if ((header_len > 0) && (body_len >= header_len)) {"
"		if ((header_len > 0) && (body_len >= header_len)
		    && (conn->phys_ctx->context_type == CONTEXT_SERVER)
		    && ((mask_len == 0) || (data_len > (uint64_t)MG_OASIS_MAX_FRAME))) {
			mg_cry_internal(conn,
			                \"%s\",
			                \"websocket frame unmasked or too large; closing connection\");
			break;
		}
		if ((header_len > 0) && (body_len >= header_len)) {")

string(APPEND text "

CIVETWEB_API void
mg_oasis_close_after(struct mg_connection *conn)
{
	if (conn) {
		conn->must_close = 1;
	}
}


CIVETWEB_API void
mg_oasis_abort(struct mg_connection *conn)
{
	if (conn) {
		conn->must_close = 1;
		if (conn->client.sock != INVALID_SOCKET) {
			(void)shutdown(conn->client.sock, SHUTDOWN_BOTH);
		}
	}
}
")
file(WRITE "${f}" "${text}")
