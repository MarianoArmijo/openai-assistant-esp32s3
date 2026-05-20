#!/usr/bin/env sh
# Interactive helper to set the env vars required by this project.
# Compatible with both bash and zsh.
#
# MUST be sourced, not executed:
#     source ./setup-env.sh
#     . ./setup-env.sh

# Detect if the script was sourced (works in bash and zsh).
_sourced=0
if [ -n "$ZSH_VERSION" ]; then
    case $ZSH_EVAL_CONTEXT in *:file*) _sourced=1 ;; esac
elif [ -n "$BASH_VERSION" ]; then
    [ "${BASH_SOURCE[0]}" != "$0" ] && _sourced=1
fi

if [ "$_sourced" -eq 0 ]; then
    echo "Error: this script must be sourced, not executed."
    echo "Run it like this:"
    echo "    source ./setup-env.sh"
    return 1 2>/dev/null || exit 1
fi

prompt_value() {
    var_name="$1"
    prompt_text="$2"
    is_secret="$3"
    current_value=$(eval "printf '%s' \"\${$var_name}\"")
    new_value=""

    if [ -n "$current_value" ]; then
        if [ "$is_secret" = "1" ]; then
            echo "${var_name} is already set (hidden). Press Enter to keep it, or type a new value."
        else
            echo "${var_name} is already set to: ${current_value}"
            echo "Press Enter to keep it, or type a new value."
        fi
    fi

    if [ "$is_secret" = "1" ]; then
        printf "%s: " "$prompt_text"
        stty -echo
        read -r new_value
        stty echo
        echo
    else
        printf "%s: " "$prompt_text"
        read -r new_value
    fi

    if [ -n "$new_value" ]; then
        eval "export ${var_name}=\"\$new_value\""
    elif [ -z "$current_value" ]; then
        echo "Error: ${var_name} cannot be empty."
        return 1
    fi
}

echo "Configuring environment variables for openai-assistant-esp32s3"
echo "--------------------------------------------------------------"

prompt_value WIFI_SSID      "WIFI SSID"       0 || return 1
prompt_value WIFI_PASSWORD  "WIFI Password"   1 || return 1
prompt_value OPENAI_API_KEY "OpenAI API Key"  1 || return 1

echo
echo "Environment configured:"
echo "  WIFI_SSID      = ${WIFI_SSID}"
echo "  WIFI_PASSWORD  = (hidden, length=${#WIFI_PASSWORD})"
echo "  OPENAI_API_KEY = (hidden, length=${#OPENAI_API_KEY})"
echo

_next_cmd="idf.py set-target esp32s3 && idf.py build flash monitor"

if [ -n "$ZSH_VERSION" ]; then
    # zsh: push the command into the next prompt's input buffer
    print -z "$_next_cmd"
    echo "Command staged in prompt — press Enter to run:"
    echo "  $ ${_next_cmd}"
elif [ -n "$BASH_VERSION" ]; then
    # bash: pre-fill the readline buffer via a key binding trick
    bind '"\er": redraw-current-line' 2>/dev/null
    bind "\"\C-x\C-a\": \"${_next_cmd}\"" 2>/dev/null
    echo "Press Ctrl-X Ctrl-A to insert and run:"
    echo "  $ ${_next_cmd}"
else
    echo "You can now run:  ${_next_cmd}"
fi

unset _next_cmd _sourced
