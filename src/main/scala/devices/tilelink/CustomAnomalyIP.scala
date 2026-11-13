// See LICENSE.SiFive for license details.

package freechips.rocketchip.devices.tilelink

import chisel3._
import chisel3.util._

import org.chipsalliance.cde.config._
import org.chipsalliance.diplomacy.lazymodule._

import freechips.rocketchip.interrupts.{HasInterruptSources, IntClockDomainCrossing}
import freechips.rocketchip.prci.{ClockSinkDomain, NoCrossing}
import freechips.rocketchip.regmapper.{RegField, RegFieldDesc, RegWriteFn, RegisterRouter, RegisterRouterParams}
import freechips.rocketchip.subsystem.{BaseSubsystem, CBUS, TLBusWrapperLocation}
import freechips.rocketchip.tilelink.{HasTLControlRegMap, TLClockDomainCrossing, TLFragmenter, TLWidthWidget}

/** Configuration parameters for the Custom Anomaly Detection IP peripheral.
  *
  * Register Map (32-bit aligned, base default 0x1002_0000):
  *   +0x00 CTRL              [R/W] : bit[0] start (WO pulse, reads 0)
  *                                   bit[1] irq_enable (R/W)
  *                                   bit[2] busy (RO)
  *   +0x04 DATA_IN           [R/W] : 32-bit input operand / sensor sample
  *   +0x08 RESULT            [RO]  : 32-bit output ((DATA_IN ^ 0xA5A55A5A) + 1) after computeCycles
  *   +0x0C IRQ_STATUS        [W1C] : bit[0] done_pending (W1C)
  *                                   bit[1] anomaly_pending (W1C)
  *                                   bits[15:8] irq_count (RO 8-bit event counter)
  *   +0x10 ANOMALY_THRESHOLD [R/W] : 32-bit upper threshold (0 = threshold check disabled)
  *   +0x14 ANOMALY_COUNT     [R/W] : 32-bit cumulative anomaly trigger counter
  */
case class CustomAnomalyIPParams(
  address: BigInt = 0x10020000L,
  size: BigInt = 0x1000,
  computeCycles: Int = 16,
  slaveWhere: TLBusWrapperLocation = CBUS
)

case object CustomAnomalyIPKey extends Field[Option[CustomAnomalyIPParams]](None)

class TLCustomAnomalyIP(params: CustomAnomalyIPParams)(implicit p: Parameters)
    extends RegisterRouter(
      RegisterRouterParams(
        name = "custom-anomaly-ip",
        compat = Seq("custom,anomaly-ip0"),
        base = params.address,
        size = params.size,
        beatBytes = 4))
    with HasTLControlRegMap
    with HasInterruptSources
{
  require(params.computeCycles >= 1, s"computeCycles must be >= 1, got ${params.computeCycles}")

  def nInterrupts: Int = 1

  lazy val module = new Impl
  class Impl extends LazyModuleImp(this) {
    val irqEn            = RegInit(0.U(1.W))
    val busy             = RegInit(0.U(1.W))
    val computeCnt       = RegInit(0.U(8.W))
    val dataIn           = RegInit(0.U(32.W))
    val latchedData      = RegInit(0.U(32.W))
    val resultReg        = RegInit(0.U(32.W))
    val donePending      = RegInit(0.U(1.W))
    val anomalyPending   = RegInit(0.U(1.W))
    val irqCount         = RegInit(0.U(8.W))
    val anomalyThreshold = RegInit(0.U(32.W))
    val anomalyCount     = RegInit(0.U(32.W))

    val startPulse         = WireDefault(false.B)
    val doneSet            = WireDefault(0.U(1.W))
    val anomalySet         = WireDefault(0.U(1.W))
    val anomalyCountWrEn   = WireDefault(false.B)
    val anomalyCountWrData = WireDefault(0.U(32.W))

    // Level-sensitive interrupt routed to TLPLIC LevelGateway via ibus
    interrupts(0) := (irqEn === 1.U) && ((donePending === 1.U) || (anomalyPending === 1.U))

    regmap(
      0x00 -> Seq(
        RegField.w(1, RegWriteFn((valid, data) => {
          when (valid && data(0) && (busy === 0.U)) {
            startPulse := true.B
          }
          true.B
        }), RegFieldDesc("start", "Write 1 to start multi-cycle compute")),
        RegField(1, irqEn,
          RegFieldDesc("irq_enable", "Enable PLIC interrupt assertion", reset = Some(0))),
        RegField.r(1, busy,
          RegFieldDesc("busy", "1 while multi-cycle compute is active", volatile = true))
      ),
      0x04 -> Seq(
        RegField(32, dataIn,
          RegFieldDesc("data_in", "32-bit input operand", reset = Some(0)))
      ),
      0x08 -> Seq(
        RegField.r(32, resultReg,
          RegFieldDesc("result", "32-bit computed result ((DATA_IN ^ 0xA5A55A5A) + 1)", reset = Some(0), volatile = true))
      ),
      0x0C -> Seq(
        RegField.w1ToClear(1, donePending, doneSet,
          Some(RegFieldDesc("done_pending", "Compute completion interrupt pending (W1C)", reset = Some(0), volatile = true))),
        RegField.w1ToClear(1, anomalyPending, anomalySet,
          Some(RegFieldDesc("anomaly_pending", "Anomaly threshold exceeded pending (W1C)", reset = Some(0), volatile = true))),
        RegField(6),
        RegField.r(8, irqCount,
          RegFieldDesc("irq_count", "8-bit IRQ event counter", reset = Some(0), volatile = true))
      ),
      0x10 -> Seq(
        RegField(32, anomalyThreshold,
          RegFieldDesc("anomaly_threshold", "Upper threshold for DATA_IN (0 = disabled)", reset = Some(0)))
      ),
      0x14 -> Seq(
        RegField(32, anomalyCount, RegWriteFn((valid, data) => {
          when (valid) {
            anomalyCountWrEn   := true.B
            anomalyCountWrData := data
          }
          true.B
        }), RegFieldDesc("anomaly_count", "Cumulative count of anomaly threshold breaches", reset = Some(0), volatile = true))
      )
    )

    // Multi-cycle compute + anomaly threshold FSM
    when (startPulse && (busy === 0.U)) {
      busy        := 1.U
      computeCnt  := params.computeCycles.U
      latchedData := dataIn
      when (anomalyCountWrEn) {
        anomalyCount := anomalyCountWrData
      }
    } .elsewhen (busy === 1.U) {
      when (computeCnt === 1.U) {
        busy       := 0.U
        computeCnt := 0.U
        resultReg  := (latchedData ^ "hA5A55A5A".U(32.W)) + 1.U
        doneSet    := 1.U
        when (irqEn === 1.U) {
          irqCount := irqCount + 1.U
        }
        val isAnomaly = (anomalyThreshold =/= 0.U) && (latchedData > anomalyThreshold)
        when (isAnomaly) {
          anomalySet   := 1.U
          anomalyCount := anomalyCount + 1.U
        } .elsewhen (anomalyCountWrEn) {
          anomalyCount := anomalyCountWrData
        }
      } .otherwise {
        computeCnt := computeCnt - 1.U
        when (anomalyCountWrEn) {
          anomalyCount := anomalyCountWrData
        }
      }
    } .elsewhen (anomalyCountWrEn) {
      anomalyCount := anomalyCountWrData
    }
  }
}

/** Trait that attaches TLCustomAnomalyIP to a RocketSubsystem (CBUS + ibus -> PLIC). */
trait CanHavePeripheryCustomAnomalyIP { this: BaseSubsystem =>
  val (customAnomalyIPOpt, customAnomalyIPDomainOpt): (Option[TLCustomAnomalyIP], Option[ClockSinkDomain]) =
    p(CustomAnomalyIPKey).map { params =>
      val tlbus = locateTLBusWrapper(params.slaveWhere)
      val domain = tlbus.generateSynchronousDomain("CustomAnomalyIP").suggestName("custom_anomaly_ip_domain")

      val dev = domain { LazyModule(new TLCustomAnomalyIP(params)) }
      domain {
        dev.controlXing(NoCrossing) := tlbus.coupleTo("custom_anomaly_ip") {
          TLFragmenter(dev.beatBytes, tlbus.blockBytes) :*= TLWidthWidget(tlbus.beatBytes) := _
        }
      }
      ibus {
        ibus.fromSync := dev.intXing(NoCrossing)
      }

      (dev, domain)
    }.unzip
}

/** Config fragment enabling TLCustomAnomalyIP at the specified MMIO base address. */
class WithCustomAnomalyIP(address: BigInt = 0x10020000L, computeCycles: Int = 16) extends Config((site, here, up) => {
  case CustomAnomalyIPKey => Some(CustomAnomalyIPParams(address = address, computeCycles = computeCycles))
})
