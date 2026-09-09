# SiLA 2 Glossary {#glossary}

Terms as the SiLA 2 specification defines them, in the order a server or client author meets them. Page numbers refer to SiLA 2 Part A v1.1 unless noted.

## SiLA Server {#gl_sila_server}
A system, software or a physical instrument, that offers Features to a SiLA Client. Every SiLA Server must implement the SiLA Service Feature.
In this library: sila2::SiLAServerBase. (Part A p.28)

## SiLA Client {#gl_sila_client}
A system, software or a physical instrument, that uses Features offered by a SiLA Server.
In this library: sila2::SilaClientBase. (Part A p.31)

## SiLA Server UUID {#gl_sila_server_uuid}
A UUID a SiLA Server generates once to uniquely identify itself. It stays the same across restarts, even after the server's lifetime has ended and a new one begins. (Part A p.29)

## Feature {#gl_feature}
A Feature describes one specific behavior of a SiLA Server, such as measuring a spectrum or controlling heating. A SiLA Server implements Features; a SiLA Client uses them. The set of Features a server offers is fixed for its whole lifetime.
In this library: sila2::SiLAServerBase::Builder::AddFeature, sila2::FeatureRegistry. (Part A p.31)

## Feature Definition / Feature Definition Language {#gl_feature_definition}
The Feature Definition describes a Feature's behavior exactly and completely: its identifier, commands, properties, data types, client metadata, and execution errors. It is written in the Feature Definition Language (FDL), an XML format validated against FeatureDefinition.xsd. (Part A p.38, p.80)

## Fully Qualified Identifier {#gl_fully_qualified_identifier}
A universally unique name for a Feature or one of its components (Command, Property, Command Parameter, Defined Execution Error, Custom Data Type, or SiLA Client Metadata), built by appending the component's own identifier to its parent's Fully Qualified Identifier. Example Feature FQI: `org.silastandard/core/SiLAService/v1`. Example Command FQI: `org.silastandard/core/SiLAService/v1/Command/GetFeatureDefinition`. (Part A p.87)

## Command {#gl_command}
A Command models an action performed on a SiLA Server. It may accept Command Parameters and may return a Command Response or, while running, Intermediate Command Responses. Every Command is either Unobservable or Observable. (Part A p.41)

## Unobservable Command {#gl_unobservable_command}
A Command whose progress cannot be, or does not need to be, observed while it runs. Its Command Response may be lost if the connection drops, since there is no way to reconnect and check on it. (Part A p.42)

## Observable Command {#gl_observable_command}
A Command whose progress or status can be observed while it runs, such as measuring a spectrum. The server returns a Command Execution UUID as soon as it accepts the command, and the client polls or subscribes for Command Execution Info and the eventual result.
In this library: sila2::ObservableCommandManager, sila2::ObservableCommandExecution. (Part A p.42)

## Command Execution UUID {#gl_command_execution_uuid}
The UUID identifying one running or completed execution of an Observable Command. It is unique within the SiLA Server instance and stays valid for the Lifetime of Execution.
In this library: sila2::ObservableCommandManager::getCommand. (Part A p.49)

## Lifetime of Execution {#gl_lifetime_of_execution}
The duration for which a Command Execution UUID stays valid. The specification measures it from the moment the server returned the UUID to the client. In this library the value passed to sila2::ObservableCommandManager::addCommand is applied after the execution finishes: the UUID stays valid for the whole run plus that duration, and a value of zero keeps it valid until the server shuts down. (Part A p.49)

## Command Execution Info / Command Execution Status {#gl_command_execution_info}
Command Execution Info reports the current state of a running Observable Command: its Command Execution Status ("Command Waiting" → "Command Running" → "Command Finished Successfully" or "Command Finished With Error", a sequence that never reverts), plus optional Progress Info and Estimated Remaining Time.
In this library: sila2::ObservableCommandExecution. (Part A p.50)

## Intermediate Command Response {#gl_intermediate_command_response}
A partial result an Observable Command may emit while still running. It is delivered by subscription, so delivery is not guaranteed; final results still belong in the Command Response. (Part A p.46)

## Property {#gl_property}
A Property describes some aspect of a SiLA Server that requires no action to read. Properties are always read-only from the client's side, even though the server may change their value on its own. Every Property is either Observable or Unobservable. (Part A p.53)

## Observable Property {#gl_observable_property}
A Property that can be read at any time and additionally offers a Property Subscription so a client is notified whenever its value changes.
In this library: sila2::ObservablePropertyManager. (Part A p.53)

## Property Subscription {#gl_property_subscription}
A SiLA Client's subscription to an Observable Property. The server sends the current value immediately on subscribing, then a new value each time it changes, until the client cancels or the connection is lost.
In this library: sila2::ObservablePropertyManager::subscribe, sila2::Subscription. (Part A p.56)

## SiLA Client Metadata {#gl_sila_client_metadata}
Small pieces of data (under 1 KB) a SiLA Server requires from the client alongside a command execution, property read, or subscription, for a concern that spans multiple Features, such as a lock identifier. If required metadata is missing, the server issues an Invalid Metadata framework error. The SiLA Service Feature is never affected by SiLA Client Metadata.
In this library: sila2::MetadataInjector. (Part A p.58)

## Validation Error {#gl_validation_error}
An error the server issues when a Command Parameter is invalid or missing, found while validating parameters before executing the Command.
In this library: sila2::error::ValidationError. (Part A p.81)

## Defined Execution Error {#gl_defined_execution_error}
An Execution Error the Feature Designer anticipated and specified as part of the Feature, identified by its own Fully Qualified Defined Execution Error Identifier. Because its cause is known in advance, a client can handle it with situation-specific recovery logic.
In this library: sila2::error::DefinedExecutionError. (Part A p.81)

## Undefined Execution Error {#gl_undefined_execution_error}
Any Execution Error that is not a Defined Execution Error: an implementation-dependent failure the Feature Designer could not foresee and so could not specify.
In this library: sila2::error::UndefinedExecutionError. (Part A p.82)

## Framework Error {#gl_framework_error}
An error the server issues when a client accesses it in a way that violates the SiLA 2 specification itself, rather than a failure of the command or property logic.
In this library: sila2::error::FrameworkError. (Part A p.82)

## SiLA Any Type {#gl_sila_any_type}
A data type that can carry a value of any SiLA Data Type except a Custom Data Type; the transmitted value carries both the data and its type. Useful when a Feature cannot know a value's type at design time, at the cost of extra overhead and implementation complexity. (Part A p.64)

## Constraint {#gl_constraint}
A restriction on the allowed value, size, or range a SiLA Data Type may take, such as Length, Pattern, or Minimal/Maximal Inclusive. The server checks every Constraint and issues a Validation Error on violation.
In this library: sila2::types::checkLength, sila2::types::checkPattern, sila2::types::checkMinimalInclusive. (Part A p.67)

## Connection Method {#gl_connection_method}
Which party establishes the Connection between client and server. With the Client-Initiated Connection Method the client connects to the server; every SiLA Server must support it. With the Server-Initiated Connection Method ("cloud connectivity" or "reverse connection"), the server connects out to the client instead. (Part A p.32)

## SiLA Server Discovery {#gl_sila_server_discovery}
A set of mechanisms a SiLA Server uses to advertise its Address on the local network, so a SiLA Client can find it without prior configuration, useful for ad-hoc lab automation setups.
In this library: sila2::discovery::MdnsPublisher. (Part A p.84)

## Binary Transfer {#gl_binary_transfer}
The mechanism for transferring binary data larger than 2 MiB between client and server, since a gRPC message field alone cannot hold it. Data is split into Binary Chunks of at most 2 MiB, referenced by a Binary Transfer UUID that is valid for its Lifetime of Binary.
In this library: sila2::BinaryStore. (Part B, Binary Transfer)

## Lock {#gl_lock}
A SiLA Client locks a SiLA Server for exclusive use by calling `LockServer` with a lock identifier. Every following (lock-protected) call must carry that identifier as SiLA Client Metadata until `UnlockServer` is called.
In this library: sila2::LockControllerImpl. (sila_base LockController-v2_0 Description)

## SiLA Service Feature / core Features {#gl_sila_service_feature}
The SiLA Service Feature is the one Feature every SiLA Server must implement; it is the entry point for discovering which other Features a server offers and for reading server identification details. Core Features such as SiLAService are standardized by the SiLA organization under the "org.silastandard" Originator and stored in the Online Feature Repository (sila_base).
In this library: sila2::SiLAServiceImpl, sila2::dynamic::FeatureCatalog. (Part A p.80, p.36)
