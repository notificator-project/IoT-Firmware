const elements = {
	select: document.querySelector('#firmware-select'),
	status: document.querySelector('#release-status'),
	board: document.querySelector('#firmware-board'),
	version: document.querySelector('#firmware-version'),
	channel: document.querySelector('#firmware-channel'),
	description: document.querySelector('#firmware-description'),
	installButton: document.querySelector('#install-button'),
	activateButton: document.querySelector('#install-button [slot="activate"]'),
	error: document.querySelector('#installer-error')
};

let releases = [];

const installerDocs = {
	deviceSetup: 'https://docs.notificator-project.com/guides/notificator-base-setup/',
	mqttSetup: 'https://docs.notificator-project.com/guides/mqtt-broker-setup/'
};

/**
 * Build the branded completion view shown inside the ESP Web Tools dialog.
 * Keeping this enhancement outside the flashing library lets us update its
 * pinned dependency without maintaining a private fork.
 */
function createInstallSuccessPanel() {
	const release = releases.find((item) => item.id === elements.select.value) || releases[0];
	const panel = document.createElement('section');
	panel.className = 'notificator-install-success';
	panel.dataset.notificatorInstallSuccess = 'true';
	panel.setAttribute('aria-labelledby', 'notificator-install-success-title');
	panel.setAttribute('aria-live', 'polite');
	panel.setAttribute('role', 'status');

	const eyebrow = document.createElement('p');
	eyebrow.className = 'notificator-install-success__eyebrow';
	eyebrow.textContent = 'Installation complete';

	const title = document.createElement('h2');
	title.id = 'notificator-install-success-title';
	title.textContent = 'Firmware installed successfully';

	const summary = document.createElement('p');
	summary.className = 'notificator-install-success__summary';
	summary.textContent = release
		? `${release.name} ${release.version} is ready. Your device will restart with a clean configuration.`
		: 'Your Notificator firmware is ready. The device will now restart with a clean configuration.';

	const nextTitle = document.createElement('h3');
	nextTitle.textContent = 'What to do next';

	const steps = document.createElement('ol');
	[
		'Wait for the device to restart. Reconnect its USB cable if the display remains blank.',
		'Join the WPNOTIF setup network. If the portal does not open, visit 192.168.4.1.',
		'Add your Wi-Fi and HiveMQ Cloud details, and keep the topic prefix set to notificator-project.'
	].forEach((instruction) => {
		const step = document.createElement('li');
		step.textContent = instruction;
		steps.append(step);
	});

	const links = document.createElement('nav');
	links.className = 'notificator-install-success__links';
	links.setAttribute('aria-label', 'Installation help');
	[
		['Device setup guide', installerDocs.deviceSetup],
		['HiveMQ setup guide', installerDocs.mqttSetup]
	].forEach(([label, url]) => {
		const link = document.createElement('a');
		link.href = url;
		link.target = '_blank';
		link.rel = 'noopener noreferrer';
		link.textContent = `${label} ↗`;
		links.append(link);
	});

	panel.append(eyebrow, title, summary, nextTitle, steps, links);
	return panel;
}

/**
 * ESP Web Tools currently renders a fixed emoji completion page and does not
 * expose a success slot. Observe its open shadow root and replace that one
 * state with project-specific guidance; serial installation remains owned by
 * the upstream component.
 */
function watchForCompletedInstall() {
	const enhanceDialog = (dialog) => {
		if (dialog.dataset.notificatorObserver === 'true') return;
		dialog.dataset.notificatorObserver = 'true';

		const observeShadowRoot = () => {
			const root = dialog.shadowRoot;
			if (!root) {
				window.requestAnimationFrame(observeShadowRoot);
				return;
			}

			const enhanceSuccessView = () => {
				const message = [...root.querySelectorAll('ewt-page-message')].find(
					(item) =>
						item.label === 'Installation complete!' ||
						item.getAttribute('label') === 'Installation complete!'
				);
				if (!message || root.querySelector('[data-notificator-install-success]')) return;

				message.replaceWith(createInstallSuccessPanel());

				if (!root.querySelector('#notificator-install-success-styles')) {
					const style = document.createElement('style');
					style.id = 'notificator-install-success-styles';
					style.textContent = `
						.notificator-install-success { color: #172033; display: grid; gap: 12px; text-align: left; }
						.notificator-install-success p, .notificator-install-success h2, .notificator-install-success h3, .notificator-install-success ol { margin: 0; }
						.notificator-install-success__eyebrow { color: #087c4c; font-size: 12px; font-weight: 800; letter-spacing: .08em; text-transform: uppercase; }
						.notificator-install-success h2 { font-size: 22px; line-height: 1.2; }
						.notificator-install-success h3 { font-size: 15px; margin-top: 4px; }
						.notificator-install-success__summary { color: #526078; line-height: 1.55; }
						.notificator-install-success ol { display: grid; gap: 9px; padding-left: 21px; color: #36445c; line-height: 1.45; }
						.notificator-install-success li::marker { color: #2f69e8; font-weight: 800; }
						.notificator-install-success__links { display: flex; flex-wrap: wrap; gap: 8px; margin-top: 3px; }
						.notificator-install-success__links a { border-radius: 999px; background: #edf3ff; color: #1857d5; font-size: 13px; font-weight: 750; padding: 8px 11px; text-decoration: none; }
						.notificator-install-success__links a:focus-visible { outline: 3px solid rgba(47, 105, 232, .35); outline-offset: 2px; }
						.notificator-install-success-close { border: 0; border-radius: 999px; background: #2f69e8; color: white; cursor: pointer; font: inherit; font-weight: 750; padding: 10px 20px; }
						.notificator-install-success-close:hover { background: #2458c7; }
						.notificator-install-success-close:focus-visible { outline: 3px solid rgba(47, 105, 232, .35); outline-offset: 2px; }
					`;
					root.append(style);
				}

				const actions = root.querySelector('div[slot="actions"]');
				const webToolsDialog = root.querySelector('ew-dialog');
				if (actions && webToolsDialog) {
					const close = document.createElement('button');
					close.className = 'notificator-install-success-close';
					close.type = 'button';
					close.textContent = 'Close installer';
					close.addEventListener('click', () => webToolsDialog.close(), {
						once: true
					});
					actions.replaceChildren(close);
					close.focus();
				}
			};

			new MutationObserver(enhanceSuccessView).observe(root, {
				childList: true,
				subtree: true
			});
			enhanceSuccessView();
		};

		observeShadowRoot();
	};

	new MutationObserver((records) => {
		records.forEach((record) => {
			record.addedNodes.forEach((node) => {
				if (!(node instanceof Element)) return;
				if (node.matches('ewt-install-dialog')) enhanceDialog(node);
				node.querySelectorAll('ewt-install-dialog').forEach(enhanceDialog);
			});
		});
	}).observe(document.body, { childList: true, subtree: true });

	document.querySelectorAll('ewt-install-dialog').forEach(enhanceDialog);
}

/**
 * Resolve a catalog path against the catalog itself. This keeps the installer
 * working on project subpaths such as GitHub Pages as well as on a custom host.
 */
function resolveCatalogPath(path) {
	return new URL(path, new URL('./firmware-catalog.json', window.location.href)).href;
}

function renderRelease(release) {
	elements.board.textContent = release.board;
	elements.version.textContent = release.version;
	elements.channel.textContent = release.channel;
	elements.description.textContent = release.description;
	elements.status.textContent = release.badge || 'Available';
	elements.installButton.manifest = resolveCatalogPath(release.manifest);
	elements.activateButton.disabled = false;
}

function showError(message) {
	elements.error.textContent = message;
	elements.error.hidden = false;
	elements.status.textContent = 'Unavailable';
	elements.select.disabled = true;
	elements.activateButton.disabled = true;
}

async function loadCatalog() {
	try {
		const response = await fetch('./firmware-catalog.json', {
			cache: 'no-store'
		});
		if (!response.ok) {
			throw new Error(`Catalog request returned ${response.status}`);
		}

		const catalog = await response.json();
		releases = catalog.firmwares.filter((release) => release.available !== false);
		if (releases.length === 0) {
			throw new Error('No installable firmware is currently published');
		}

		elements.select.replaceChildren(
			...releases.map((release) => {
				const option = document.createElement('option');
				option.value = release.id;
				option.textContent = `${release.name} · ${release.board}`;
				return option;
			})
		);
		elements.select.disabled = false;
		renderRelease(releases[0]);
	} catch (error) {
		console.error('[Notificator installer] Unable to load firmware catalog.', error);
		showError('The firmware catalog could not be loaded. Please refresh the page or try again later.');
	}
}

elements.select.addEventListener('change', (event) => {
	const release = releases.find((item) => item.id === event.target.value);
	if (release) {
		renderRelease(release);
	}
});

loadCatalog();
watchForCompletedInstall();
