/**
 * Simple i18n system for thingino web UI
 * Loads language once on page load, no real-time switching
 */

class SimpleI18n {
    constructor() {
        this.translations = {};
        this.currentLanguage = 'en';
        this.fallbackLanguage = 'en';
        this.isLoaded = false;
    }

    /**
     * Initialize i18n system
     * Loads current language from server
     */
    async init() {
        try {
            // Load current language file
            const response = await fetch('/opt/webui/i18n/current.json');
            if (response.ok) {
                this.translations = await response.json();
                this.currentLanguage = this.translations.language || 'en';
                this.isLoaded = true;
                console.log(`i18n: Loaded language ${this.currentLanguage}`);
                
                // Apply translations to page
                this.translatePage();
                
                return true;
            } else {
                console.warn('i18n: Failed to load current language, using fallback');
                return this.loadFallback();
            }
        } catch (error) {
            console.error('i18n: Error loading language:', error);
            return this.loadFallback();
        }
    }

    /**
     * Load fallback language (English)
     */
    async loadFallback() {
        try {
            const response = await fetch('/opt/webui/i18n/en.json');
            if (response.ok) {
                this.translations = await response.json();
                this.currentLanguage = 'en';
                this.isLoaded = true;
                this.translatePage();
                return true;
            }
        } catch (error) {
            console.error('i18n: Failed to load fallback language:', error);
        }
        return false;
    }

    /**
     * Get translation for a key
     * @param {string} key - Translation key (supports dot notation)
     * @param {object} params - Parameters for string interpolation
     * @returns {string} Translated string
     */
    t(key, params = {}) {
        if (!this.isLoaded) {
            return key; // Return key if not loaded yet
        }

        let translation = this.getNestedValue(this.translations, key);
        
        if (translation === undefined) {
            console.warn(`i18n: Missing translation for key: ${key}`);
            return key;
        }

        // Simple parameter substitution
        if (typeof translation === 'string' && Object.keys(params).length > 0) {
            Object.keys(params).forEach(param => {
                translation = translation.replace(new RegExp(`{${param}}`, 'g'), params[param]);
            });
        }

        return translation;
    }

    /**
     * Get nested value from object using dot notation
     * @param {object} obj - Object to search
     * @param {string} path - Dot notation path
     * @returns {any} Value or undefined
     */
    getNestedValue(obj, path) {
        return path.split('.').reduce((current, key) => {
            return current && current[key] !== undefined ? current[key] : undefined;
        }, obj);
    }

    /**
     * Translate all elements on the page
     */
    translatePage() {
        // Translate elements with data-i18n attribute
        document.querySelectorAll('[data-i18n]').forEach(element => {
            const key = element.getAttribute('data-i18n');
            const translation = this.t(key);
            
            if (element.tagName === 'INPUT' && (element.type === 'submit' || element.type === 'button')) {
                element.value = translation;
            } else if (element.hasAttribute('placeholder')) {
                element.placeholder = translation;
            } else {
                element.textContent = translation;
            }
        });

        // Translate elements with data-i18n-title attribute
        document.querySelectorAll('[data-i18n-title]').forEach(element => {
            const key = element.getAttribute('data-i18n-title');
            element.title = this.t(key);
        });

        // Translate elements with data-i18n-placeholder attribute
        document.querySelectorAll('[data-i18n-placeholder]').forEach(element => {
            const key = element.getAttribute('data-i18n-placeholder');
            element.placeholder = this.t(key);
        });

        console.log('i18n: Page translation complete');
    }

    /**
     * Get current language code
     * @returns {string} Current language code
     */
    getCurrentLanguage() {
        return this.currentLanguage;
    }

    /**
     * Check if i18n is loaded
     * @returns {boolean} True if loaded
     */
    isReady() {
        return this.isLoaded;
    }

    /**
     * Get available languages from server
     * @returns {Promise<object>} Available languages
     */
    async getAvailableLanguages() {
        try {
            const response = await fetch('/opt/webui/i18n/available.json');
            if (response.ok) {
                return await response.json();
            }
        } catch (error) {
            console.error('i18n: Error loading available languages:', error);
        }
        
        // Fallback
        return {
            languages: [
                {code: 'en', name: 'English', native: 'English'}
            ],
            default: 'en'
        };
    }
}

// Global i18n instance
const i18n = new SimpleI18n();

// Global translation function
function t(key, params = {}) {
    return i18n.t(key, params);
}

// Initialize when DOM is ready
if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', () => i18n.init());
} else {
    i18n.init();
}

// Export for module systems
if (typeof module !== 'undefined' && module.exports) {
    module.exports = { SimpleI18n, i18n, t };
}
