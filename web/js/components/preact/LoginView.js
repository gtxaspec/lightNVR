/**
 * LightNVR Web Interface LoginView Component
 * Preact component for the login page
 */

import { h } from '../../preact.min.js';
import { html } from '../../html-helper.js';
import { useState, useRef } from '../../preact.hooks.module.js';
import { showStatusMessage } from './UI.js';
import { enhancedFetch, createRequestController } from '../../fetch-utils.js';

/**
 * LoginView component
 * @returns {JSX.Element} LoginView component
 */
export function LoginView() {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [isLoggingIn, setIsLoggingIn] = useState(false);
  const [errorMessage, setErrorMessage] = useState('');
  
  // Check URL for error or auth_required parameter
  useState(() => {
    const urlParams = new URLSearchParams(window.location.search);
    if (urlParams.has('error')) {
      setErrorMessage('Invalid username or password');
    } else if (urlParams.has('auth_required')) {
      setErrorMessage('Authentication required. Please log in to continue.');
    }
  }, []);
  
  // Request controller for cancelling requests
  const requestControllerRef = useRef(null);

  // Handle login form submission
  const handleLogin = async (e) => {
    e.preventDefault();
    
    if (!username || !password) {
      setErrorMessage('Please enter both username and password');
      return;
    }
    
    setIsLoggingIn(true);
    
    // Create a new request controller
    requestControllerRef.current = createRequestController();
    
    // Store credentials in localStorage for future requests
    const auth = btoa(`${username}:${password}`);
    localStorage.setItem('auth', auth);
    
    try {
      // Make a fetch request to the login API using enhanced fetch
      const response = await enhancedFetch('/api/auth/login', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': 'Basic ' + auth,
          // Add additional headers for Firefox compatibility
          'X-Requested-With': 'XMLHttpRequest',
          'Accept': 'application/json'
        },
        body: JSON.stringify({ username, password }),
        credentials: 'include',
        mode: 'same-origin', // Explicitly set mode for Firefox
        signal: requestControllerRef.current?.signal,
        timeout: 5000,     // 5 second timeout
        retries: 1,        // Retry once
        retryDelay: 1000   // 1 second between retries
      });
      
      if (response.ok || response.status === 302) {
        // Successful login - redirect to live page
        window.location.href = '/index.html?t=' + new Date().getTime();
      } else {
        // Failed login
        setIsLoggingIn(false);
        setErrorMessage('Invalid username or password');
        localStorage.removeItem('auth');
      }
    } catch (error) {
      console.error('Login error:', error);
      
      // If it's a timeout error, proceed anyway with stored credentials
      if (error.message === 'Request timed out' && localStorage.getItem('auth')) {
        console.log('Login request timed out, proceeding with stored credentials');
        window.location.href = '/index.html?t=' + new Date().getTime();
      } 
      // For other errors, also try to proceed if we have credentials
      else if (localStorage.getItem('auth')) {
        console.log('Login API error, but proceeding with stored credentials');
        window.location.href = '/index.html?t=' + new Date().getTime();
      } else {
        setIsLoggingIn(false);
        setErrorMessage('Login failed. Please try again.');
      }
    }
  };
  
  return html`
    <section id="login-page" class="page flex items-center justify-center min-h-screen">
      <div class="login-container w-full max-w-md p-6 bg-white dark:bg-gray-800 rounded-lg shadow-lg">
        <div class="text-center mb-8">
          <h1 class="text-2xl font-bold">LightNVR</h1>
          <p class="text-gray-600 dark:text-gray-400">Please sign in to continue</p>
        </div>
        
        ${errorMessage && html`
          <div class="mb-4 p-3 bg-red-100 text-red-700 dark:bg-red-900 dark:text-red-200 rounded-lg">
            ${errorMessage}
          </div>
        `}
        
        <form id="login-form" class="space-y-6" action="/api/auth/login" method="POST" onSubmit=${handleLogin}>
          <div class="form-group">
            <label for="username" class="block text-sm font-medium mb-1">Username</label>
            <input 
              type="text" 
              id="username" 
              name="username"
              class="w-full px-3 py-2 border border-gray-300 rounded-md shadow-sm focus:outline-none focus:ring-blue-500 focus:border-blue-500 dark:bg-gray-700 dark:border-gray-600 dark:text-white"
              placeholder="Enter your username" 
              value=${username}
              onChange=${e => setUsername(e.target.value)}
              required
              autocomplete="username"
            />
          </div>
          <div class="form-group">
            <label for="password" class="block text-sm font-medium mb-1">Password</label>
            <input 
              type="password" 
              id="password" 
              name="password"
              class="w-full px-3 py-2 border border-gray-300 rounded-md shadow-sm focus:outline-none focus:ring-blue-500 focus:border-blue-500 dark:bg-gray-700 dark:border-gray-600 dark:text-white"
              placeholder="Enter your password" 
              value=${password}
              onChange=${e => setPassword(e.target.value)}
              required
              autocomplete="current-password"
            />
          </div>
          <div class="form-group">
            <button 
              type="submit" 
              class="w-full px-4 py-2 bg-blue-600 text-white rounded hover:bg-blue-700 transition-colors focus:outline-none focus:ring-2 focus:ring-blue-500 focus:ring-offset-2 dark:focus:ring-offset-gray-800 disabled:opacity-50 disabled:cursor-not-allowed"
              disabled=${isLoggingIn}
            >
              ${isLoggingIn ? 'Signing in...' : 'Sign In'}
            </button>
          </div>
        </form>
        
        <div class="mt-6 text-center text-sm text-gray-600 dark:text-gray-400">
          <p>Default credentials: admin / admin</p>
          <p class="mt-2">You can change these in Settings after login</p>
        </div>
      </div>
    </section>
  `;
}

/**
 * Load LoginView component
 */
export function loadLoginView() {
  const mainContent = document.getElementById('main-content');
  if (!mainContent) return;
  
  // Render the LoginView component to the container
  import('../../preact.min.js').then(({ render }) => {
    render(html`<${LoginView} />`, mainContent);
  });
}
