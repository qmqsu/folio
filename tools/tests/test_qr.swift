// Independently decode the actual native grayscale screenshot with Apple Vision.
// This uses a fixed test-only session URL, never device credentials.
import Foundation
import Vision

guard CommandLine.arguments.count == 2 else { fatalError("Expected transfer screenshot path") }
let request = VNDetectBarcodesRequest()
request.revision = VNDetectBarcodesRequestRevision1
request.usesCPUOnly = true
request.symbologies = [.qr]
try VNImageRequestHandler(url: URL(fileURLWithPath: CommandLine.arguments[1])).perform([request])
let values = request.results?.compactMap { $0.payloadStringValue } ?? []
precondition(values == ["http://192.168.4.1/#0123456789abcdef0123456789abcdef"], "QR payload differs from the authenticated upload URL")
print("Native QR screenshot decoded to the exact test session URL")
