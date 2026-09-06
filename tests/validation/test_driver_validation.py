"""Black-box SiLA 2 spec-compliance tests against the interop server."""

import pytest

sila2 = pytest.importorskip("sila2", reason="sila2 package not installed")
from sila2.framework import DefinedExecutionError, FrameworkError


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_client(sila_server):
    """Connect a SilaClient to the running interop server (sila2 v0.14.0+)."""
    from sila2.client import SilaClient

    return SilaClient(
        sila_server["host"],
        sila_server["port"],
        root_certs=sila_server.get("root_certs"),
    )


# ---------------------------------------------------------------------------
# Connection & discovery
# ---------------------------------------------------------------------------

class TestServerDiscovery:
    """Verify the server is reachable and exposes SiLA 2 metadata."""

    def test_client_connects(self, sila_server):
        client = _make_client(sila_server)
        assert client is not None
        client.close()

    def test_server_has_address_and_port(self, sila_server):
        client = _make_client(sila_server)
        assert client.address == sila_server["host"]
        assert client.port == sila_server["port"]
        client.close()

    def test_server_name_not_empty(self, sila_server):
        client = _make_client(sila_server)
        name = client.SiLAService.ServerName.get()
        assert name, "Server name must not be empty"
        client.close()


# ---------------------------------------------------------------------------
# Feature introspection
# ---------------------------------------------------------------------------

class TestFeatureDefinition:
    """Verify Feature definitions are retrievable and well-formed."""

    def test_get_implemented_features(self, sila_server):
        client = _make_client(sila_server)
        features = client.SiLAService.ImplementedFeatures.get()
        feature_ids = list(features)
        assert len(feature_ids) > 0, "Server must implement at least one Feature"
        client.close()

    def test_get_feature_definition(self, sila_server):
        client = _make_client(sila_server)
        features = client.SiLAService.ImplementedFeatures.get()
        for feature in features:
            defn = client.SiLAService.GetFeatureDefinition(feature)
            assert defn is not None, f"No definition for {feature}"
        client.close()

    def test_feature_id_format(self, sila_server):
        """SiLA 2 Feature IDs follow the qualified identifier format."""
        client = _make_client(sila_server)
        features = client.SiLAService.ImplementedFeatures.get()
        for feature in features:
            parts = feature.split("/")
            assert len(parts) >= 3, (
                f"Feature ID '{feature}' does not match "
                "org/category/FeatureName/vN format"
            )
        client.close()

    def test_feature_definition_contains_xml(self, sila_server):
        """Each Feature definition must be valid FDL (XML)."""
        client = _make_client(sila_server)
        features = client.SiLAService.ImplementedFeatures.get()
        for feature in features:
            fdl = client.SiLAService.GetFeatureDefinition(feature)
            fdl_xml = fdl.FeatureDefinition if isinstance(fdl.FeatureDefinition, str) else fdl.FeatureDefinition.value
            assert "Feature" in fdl_xml, (
                f"FDL for {feature} does not contain <Feature> element"
            )
        client.close()

    def test_server_uuid_is_uuid(self, sila_server):
        """ServerUUID must be a valid UUID string."""
        import uuid
        client = _make_client(sila_server)
        server_uuid = client.SiLAService.ServerUUID.get()
        uuid.UUID(server_uuid if isinstance(server_uuid, str) else server_uuid.value)
        client.close()


# ---------------------------------------------------------------------------
# Python client interoperability
# ---------------------------------------------------------------------------

class TestPythonClientInterop:
    """Exercise C++ feature handlers through the public sila2 client API."""

    def test_unobservable_command_echoes_binary(self, sila_server):
        with _make_client(sila_server) as client:
            response = client.BinaryTransferTest.EchoBinaryValue(b"python-client")
            assert response.ReceivedValue == b"python-client"

    def test_property_gets_binary_value(self, sila_server):
        with _make_client(sila_server) as client:
            value = client.BinaryTransferTest.BinaryValueDirectly.get()
            assert value == b"SiLA2_Test_String_Value"

    def test_login_reports_declared_authentication_failure(self, sila_server):
        with _make_client(sila_server) as client:
            with pytest.raises(DefinedExecutionError) as raised:
                client.AuthenticationService.Login(
                    "wrong", "wrong", client.SiLAService.ServerUUID.get(), []
                )
            assert type(raised.value) is DefinedExecutionError
            assert str(raised.value.fully_qualified_identifier) == (
                "org.silastandard/core/AuthenticationService/v1/DefinedExecutionError/"
                "AuthenticationFailed"
            )

    def test_observable_command_returns_intermediates_and_result(self, sila_server):
        with _make_client(sila_server) as client:
            execution = client.BinaryTransferTest.EchoBinariesObservably([b"first", b"second"])
            try:
                with execution.subscribe_to_intermediate_responses() as responses:
                    assert [response.Binary for response in responses] == [b"first", b"second"]
                assert execution.get_responses().JointBinary == b"firstsecond"
            finally:
                execution.cancel_execution_info_subscription()

    def test_login_authorizes_protected_command(self, sila_server):
        with _make_client(sila_server) as client:
            with pytest.raises(FrameworkError):
                client.AuthenticationTest.RequiresToken()
            login = client.AuthenticationService.Login(
                "test", "test", client.SiLAService.ServerUUID.get(), []
            )
            response = client.AuthenticationTest.RequiresToken(
                metadata=[client.AuthorizationService.AccessToken(login.AccessToken)]
            )
            assert response == ()
