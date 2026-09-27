/*
 * Planner: notes, a schedule and tables, stored on the system drive under
 * usbstore/planner/ as one JSON document each.
 *
 * The firmware never parses them. Each document is an opaque blob that the
 * browser loads, edits and saves whole; all the logic lives in the page.
 * That keeps the ESP side small and impossible to confuse with odd content,
 * and it's cheap: a year of notes is a few hundred KB.
 *
 * Documents:
 *   notes, events, tables  admin password to read or write
 *   upcoming               derived by the page on every schedule save: the
 *                          next occurrences of every event (repeats expanded)
 *                          so Desk-Disp can show the next tasks without
 *                          understanding repeat rules. Readable with the
 *                          device key, writable only by the admin.
 */

#pragma once

#include "esp_http_server.h"

void planner_routes(httpd_handle_t srv);
