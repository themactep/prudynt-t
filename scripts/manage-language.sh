#!/bin/bash

# Language management script for thingino web UI
# Handles downloading and switching languages

I18N_DIR="/opt/webui/i18n"
GITHUB_BASE="https://raw.githubusercontent.com/thingino/thingino-firmware/master/package/thingino-webui/files/var/www/a/i18n"
LOG_FILE="/tmp/i18n_setup.log"

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') - $1" | tee -a "$LOG_FILE"
}

# Function to download a language file
download_language() {
    local lang="$1"
    local temp_file="/tmp/${lang}.json.tmp"
    
    log "Downloading language: $lang"
    
    if wget -q --timeout=10 "$GITHUB_BASE/$lang.json" -O "$temp_file"; then
        # Validate JSON
        if jq empty "$temp_file" 2>/dev/null; then
            mv "$temp_file" "$I18N_DIR/$lang.json"
            log "Successfully downloaded and validated: $lang"
            return 0
        else
            log "Invalid JSON downloaded for: $lang"
            rm -f "$temp_file"
            return 1
        fi
    else
        log "Failed to download: $lang"
        rm -f "$temp_file"
        return 1
    fi
}

# Function to set current language
set_language() {
    local lang="$1"
    
    if [ ! -f "$I18N_DIR/$lang.json" ]; then
        log "Language file not found: $lang.json, attempting download"
        if ! download_language "$lang"; then
            log "Failed to download language: $lang"
            return 1
        fi
    fi
    
    # Create symlink to current language
    ln -sf "$I18N_DIR/$lang.json" "$I18N_DIR/current.json"
    
    # Store current language setting
    echo "$lang" > "$I18N_DIR/current_lang"
    
    log "Language set to: $lang"
    return 0
}

# Function to get current language
get_current_language() {
    if [ -f "$I18N_DIR/current_lang" ]; then
        cat "$I18N_DIR/current_lang"
    else
        echo "en"  # Default fallback
    fi
}

# Function to list available languages
list_languages() {
    if [ -f "$I18N_DIR/available.json" ]; then
        cat "$I18N_DIR/available.json"
    else
        echo '{"languages":[],"default":"en"}'
    fi
}

# Function to update all language files
update_all() {
    log "Updating all language files from GitHub"
    
    # Read available languages
    local languages=($(jq -r '.languages[].code' "$I18N_DIR/available.json" 2>/dev/null || echo "en es"))
    
    for lang in "${languages[@]}"; do
        download_language "$lang"
    done
    
    log "Language update complete"
}

# Main command handling
case "$1" in
    "set")
        if [ -z "$2" ]; then
            echo "Usage: $0 set <language_code>"
            exit 1
        fi
        set_language "$2"
        ;;
    "get")
        get_current_language
        ;;
    "list")
        list_languages
        ;;
    "download")
        if [ -z "$2" ]; then
            echo "Usage: $0 download <language_code>"
            exit 1
        fi
        download_language "$2"
        ;;
    "update")
        update_all
        ;;
    "init")
        # Initialize the system
        mkdir -p "$I18N_DIR"
        if [ ! -f "$I18N_DIR/available.json" ]; then
            # Create initial available languages list
            cat > "$I18N_DIR/available.json" << 'EOF'
{
    "languages": [
        {"code": "en", "name": "English", "native": "English"},
        {"code": "es", "name": "Spanish", "native": "Español"}
    ],
    "default": "en"
}
EOF
        fi
        set_language "en"
        ;;
    *)
        echo "Usage: $0 {set|get|list|download|update|init} [language_code]"
        echo ""
        echo "Commands:"
        echo "  set <lang>     Set current language"
        echo "  get            Get current language"
        echo "  list           List available languages"
        echo "  download <lang> Download specific language"
        echo "  update         Update all languages from GitHub"
        echo "  init           Initialize i18n system"
        exit 1
        ;;
esac
