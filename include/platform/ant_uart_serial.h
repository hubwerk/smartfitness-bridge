#pragma once

#if defined(SF_ANT_UART_TX) != defined(SF_ANT_UART_RX)
#error "Define both SF_ANT_UART_TX and SF_ANT_UART_RX to enable ANT UART"
#endif

#if defined(ARDUINO) && defined(SF_ANT_UART_TX) && defined(SF_ANT_UART_RX)
#include "core/ant/ant_uart_transport.hpp"

// Optional SF_ANT_UART_RTS wires the module's active-low nRTS output to host CTS.
// Optional SF_ANT_UART_RESET is the module's active-low reset input.
smartfitness::ant::AntUartTransport& antUartTransport();
#endif
