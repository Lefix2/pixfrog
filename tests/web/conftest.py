def pytest_configure(config):
    config.addinivalue_line("markers", "device_args(*args): extra pixfrog_api_host arguments")
