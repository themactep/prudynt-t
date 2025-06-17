#!/bin/bash

# Setup script for new i18n system
# Creates directory structure and downloads initial language files

I18N_DIR="/opt/webui/i18n"
GITHUB_BASE="https://raw.githubusercontent.com/thingino/thingino-firmware/master/package/thingino-webui/files/var/www/a/i18n"
DEFAULT_LANG="en"

echo "Setting up i18n system..."

# Create directory structure
mkdir -p "$I18N_DIR"

# Available languages
LANGUAGES=("en" "es" "fr" "de" "ru" "zh")

# Download language files from GitHub
for lang in "${LANGUAGES[@]}"; do
    echo "Downloading $lang.json..."
    if wget -q "$GITHUB_BASE/$lang.json" -O "$I18N_DIR/$lang.json"; then
        echo "✓ Downloaded $lang.json"
    else
        echo "✗ Failed to download $lang.json"
        # Create minimal fallback if download fails
        echo '{"language":"'$lang'","name":"'$lang'"}' > "$I18N_DIR/$lang.json"
    fi
done

# Set default language
if [ -f "$I18N_DIR/$DEFAULT_LANG.json" ]; then
    ln -sf "$I18N_DIR/$DEFAULT_LANG.json" "$I18N_DIR/current.json"
    echo "✓ Set default language to $DEFAULT_LANG"
else
    echo "✗ Default language file not found"
    exit 1
fi

# Create language list file
cat > "$I18N_DIR/available.json" << EOF
{
    "languages": [
        {"code": "en", "name": "English", "native": "English"},
        {"code": "es", "name": "Spanish", "native": "Español"},
        {"code": "fr", "name": "French", "native": "Français"},
        {"code": "de", "name": "German", "native": "Deutsch"},
        {"code": "ru", "name": "Russian", "native": "Русский"},
        {"code": "zh", "name": "Chinese", "native": "中文"}
    ],
    "default": "$DEFAULT_LANG"
}
EOF

echo "✓ Created available languages list"

# Set permissions
chmod 755 "$I18N_DIR"
chmod 644 "$I18N_DIR"/*.json

echo "✓ i18n setup complete!"
echo "Language files stored in: $I18N_DIR"
echo "Current language: $(readlink $I18N_DIR/current.json)"
