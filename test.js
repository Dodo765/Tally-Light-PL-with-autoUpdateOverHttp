function fetchSwitchState(state) {
	const urlslong = "http://192.168.100.94/save";

	const urls = urlslong.split(",");

	const switcherValue = state;

	urls.forEach((url) => {
		const requestOptions = {
			method: "POST",
			headers: {
				"Content-Type": "application/x-www-form-urlencoded",
			},
			body: `switcher=${switcherValue}`,
		};
		fetch(url, requestOptions)
			.then((response) => {
				if (!response.ok) {
					throw new Error(`Błąd HTTP! Kod: ${response.status}`);
				}
				return response.text();
			})
			.then((data) => {
				console.log(`Odpowiedź z ${url}: ${data}`);
			})
			.catch((error) => {
				console.error(`Błąd: ${error.message}`);
			});
	});
}
