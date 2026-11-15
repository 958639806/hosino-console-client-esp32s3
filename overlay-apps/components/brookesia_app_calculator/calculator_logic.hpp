/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 * The official Calculator's signed-term stack and decimal/percent parser,
 * bounded and validated for a small embedded UI.
 */
#pragma once
#include <cmath>
#include <cstddef>
#include <limits>

namespace esp_brookesia::apps::calculator {
inline double calculate(const char *input)
{
    const double error = std::numeric_limits<double>::quiet_NaN();
    if (!input || !*input) return error;
    double stack[49] = {};
    size_t count = 0, consumed = 0;
    char previous = '+';
    const char *p = input;
    if (*p == '+' || *p == '-') previous = *p++;
    while (*p) {
        double number = 0;
        double fraction = 0.1;
        bool decimal = false, have_digit = false;
        while ((*p >= '0' && *p <= '9') || *p == '.') {
            if (++consumed > 96) return error;
            if (*p == '.') {
                if (decimal) return error;
                decimal = true;
            } else {
                have_digit = true;
                if (decimal) { number += (*p - '0') * fraction; fraction *= 0.1; }
                else number = number * 10 + (*p - '0');
            }
            ++p;
        }
        if (!have_digit) return error;
        if (*p == '%') { number /= 100.0; ++p; ++consumed; }
        switch (previous) {
        case '+':
        case '-':
            if (count == 49) return error;
            stack[count++] = previous == '+' ? number : -number;
            break;
        case 'x':
            if (!count) return error;
            stack[count - 1] *= number;
            break;
        case '/':
            if (!count || number == 0) return error;
            stack[count - 1] /= number;
            break;
        default: return error;
        }
        if (!std::isfinite(stack[count - 1])) return error;
        if (!*p) break;
        if (*p != '+' && *p != '-' && *p != 'x' && *p != '/') return error;
        previous = *p++;
        if (++consumed > 96 || !*p) return error;
    }
    if (!count) return error;
    double result = 0;
    for (size_t i = 0; i < count; ++i) result += stack[i];
    return result;
}
} // namespace esp_brookesia::apps::calculator
