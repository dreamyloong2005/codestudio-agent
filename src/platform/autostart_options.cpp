#include "codestudio/autostart.hpp"
#include <stdexcept>

namespace codestudio {
void validate_autostart_options(const AutostartOptions& options) {
    if (options.poll_ms<20 || options.poll_ms>5000)
        throw std::invalid_argument("Autostart poll-ms must be in 20..5000");
    if (options.max_restarts<0 || options.max_restarts>100 ||
        options.restart_delay_ms<100 || options.restart_delay_ms>60000)
        throw std::invalid_argument("Invalid autostart supervision limits");
    if (!options.supervise && (options.max_restarts!=3 || options.restart_delay_ms!=1000))
        throw std::invalid_argument("Autostart restart limits require supervision");
}
}
